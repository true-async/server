/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

/* Keys follow PHP's rules for $_POST (http_form_vars.h): array notation,
 * the input filter, max_input_vars, max_input_nesting_level, and a dot or a
 * space in a base name turned into `_`. An url-encoded body splits on `&`
 * alone, as $_POST does. */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "php.h"
#include "Zend/zend_async_API.h"
#include "Zend/zend_exceptions.h"
#include "php_http_server.h"
#include "http_form_vars.h"
#include "http1/http_parser.h"
#include "http_request_form.h"
#include "formats/form_content_type.h"
#include "formats/multipart_processor.h"

#include <string.h>

/* uploaded_file.c; an emalloc'd zval the caller frees after moving the object out. */
extern zval *uploaded_file_create_from_info(mp_file_info_t *info);

mp_processor_t *http_request_form_open_multipart(const http_request_t *req,
												 struct http_log_state *log_state)
{
	const zval *const content_type =
		zend_hash_str_find(req->headers, "content-type", sizeof("content-type") - 1);

	if (content_type == NULL || Z_TYPE_P(content_type) != IS_STRING) {
		return NULL;
	}

	const char *boundary;
	size_t boundary_len;

	if (!http_form_find_boundary(Z_STRVAL_P(content_type), Z_STRLEN_P(content_type), &boundary,
								 &boundary_len)) {
		return NULL;
	}

	/* The Content-Type check and the multipart parser hold one limit (RFC 2046). */
	ZEND_STATIC_ASSERT(HTTP_FORM_BOUNDARY_MAX_LEN == MULTIPART_MAX_BOUNDARY_LEN,
					   "boundary limits disagree");
	char terminated[HTTP_FORM_BOUNDARY_MAX_LEN + 1];

	memcpy(terminated, boundary, boundary_len);
	terminated[boundary_len] = '\0';

	mp_processor_t *const processor = mp_processor_create(terminated, NULL);

	if (processor != NULL) {
		processor->log_state = log_state;
	}

	return processor;
}

static void http_request_form_decode_urlencoded(const http_request_t *req, zval *post)
{
	if (req->body == NULL || ZSTR_LEN(req->body) == 0) {
		return;
	}

	http_form_vars_decode(Z_ARRVAL_P(post), ZSTR_VAL(req->body), ZSTR_LEN(req->body), "&",
						  PG(max_input_vars), PG(max_input_nesting_level));
}

/* A buffered multipart body (HTTP/3) is fed in slices, and the handler yields
 * between them. The processor writes file parts synchronously, and on HTTP/3
 * the handler shares its thread with the QUIC transport, whose ACKs one long
 * span would delay for every connection (CODING_STANDARDS 1.5). */
#define HTTP_REQUEST_FORM_FEED_SLICE (256u * 1024u)

/* False when the yield ended in an exception, a cancellation among them. */
static bool http_request_form_yield(void)
{
	zend_coroutine_t *const coroutine = ZEND_ASYNC_CURRENT_COROUTINE;

	/* No coroutine to yield: http_parse_request() outside the server. */
	if (coroutine == NULL || ZEND_ASYNC_IS_SCHEDULER_CONTEXT) {
		return true;
	}

	ZEND_ASYNC_ENQUEUE_COROUTINE(coroutine);
	ZEND_ASYNC_SUSPEND();
	zend_async_waker_clean(coroutine);

	return EG(exception) == NULL;
}

/* The processor of a buffered body in `*processor`, NULL when the body is
 * absent or the processor refuses it. False when a yield ended in an
 * exception; nothing is kept then. */
static bool http_request_form_parse_buffered(const http_request_t *req, mp_processor_t **processor)
{
	*processor = NULL;

	if (req->body == NULL) {
		return true;
	}

	mp_processor_t *const parsing = http_request_form_open_multipart(req, NULL);

	if (parsing == NULL) {
		return true;
	}

	const char *next = ZSTR_VAL(req->body);
	size_t remaining = ZSTR_LEN(req->body);

	while (remaining > 0) {
		const size_t slice =
			remaining < HTTP_REQUEST_FORM_FEED_SLICE ? remaining : HTTP_REQUEST_FORM_FEED_SLICE;

		if (mp_processor_feed(parsing, next, slice) < 0) {
			mp_processor_cleanup_temp_files(parsing);
			mp_processor_destroy(parsing);
			return true;
		}

		next += slice;
		remaining -= slice;

		if (remaining > 0 && !http_request_form_yield()) {
			mp_processor_cleanup_temp_files(parsing);
			mp_processor_destroy(parsing);
			return false;
		}
	}

	*processor = parsing;
	return true;
}

static void http_request_form_publish(const mp_processor_t *processor, zval *post, zval *files)
{
	size_t field_count;
	const mp_field_info_t *const fields = mp_processor_get_fields(processor, &field_count);

	zend_long field_budget = PG(max_input_vars);

	for (size_t i = 0; i < field_count && field_budget > 0; i++) {
		if (fields[i].name == NULL) {
			continue;
		}

		char *value = estrndup(fields[i].value, fields[i].value_len);
		size_t filtered_len;

		field_budget--;

		if (http_form_vars_filter(fields[i].name, &value, fields[i].value_len, &filtered_len)) {
			zval stored;

			ZVAL_STRINGL_FAST(&stored, value, filtered_len);
			http_form_vars_register(Z_ARRVAL_P(post), fields[i].name, strlen(fields[i].name),
									&stored, PG(max_input_nesting_level));
		}

		efree(value);
	}

	size_t upload_count;
	mp_file_info_t *const uploads = mp_processor_get_files(processor, &upload_count);

	for (size_t i = 0; i < upload_count; i++) {
		if (uploads[i].field_name == NULL) {
			continue;
		}

		zval *const upload = uploaded_file_create_from_info(&uploads[i]);

		http_form_vars_register(Z_ARRVAL_P(files), uploads[i].field_name,
								strlen(uploads[i].field_name), upload, PG(max_input_nesting_level));
		efree(upload);
	}
}

static bool http_request_form_decode_multipart(http_request_t *req, zval *post, zval *files)
{
	if (req->multipart_proc == NULL &&
		!http_request_form_parse_buffered(req, &req->multipart_proc)) {
		return false;
	}

	if (req->multipart_proc != NULL) {
		http_request_form_publish(req->multipart_proc, post, files);
	}

	return true;
}

bool http_request_form_build(http_request_t *req)
{
	/* Another coroutine of this request is parsing the body, yielding between
	 * slices: wait for its form rather than parse the body a second time. */
	if (req->form_building &&
		(ZEND_ASYNC_CURRENT_COROUTINE == NULL || ZEND_ASYNC_IS_SCHEDULER_CONTEXT)) {
		/* Nothing to yield with, so the builder could never finish. */
		zend_throw_exception(http_server_runtime_exception_ce,
							 "the form is being built by another coroutine, and this context "
							 "cannot wait for it",
							 0);
		return false;
	}

	while (req->form_building) {
		if (!http_request_form_yield()) {
			return false;
		}
	}

	if (req->post_data != NULL || !req->complete) {
		return true;
	}

	zval post;
	zval files;
	bool built = true;

	array_init(&post);
	array_init(&files);
	req->form_building = true;

	if (req->form_kind == HTTP_FORM_URLENCODED) {
		http_request_form_decode_urlencoded(req, &post);
	} else if (req->form_kind == HTTP_FORM_MULTIPART) {
		built = http_request_form_decode_multipart(req, &post, &files);
	}

	req->form_building = false;

	if (!built) {
		zval_ptr_dtor(&post);
		zval_ptr_dtor(&files);
		return false;
	}

	req->post_data = Z_ARR(post);
	req->files = Z_ARR(files);
	return true;
}
