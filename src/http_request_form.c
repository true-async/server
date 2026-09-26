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

	/* Fields stop at max_input_vars, as url-encoded ones do; the processor
	 * cannot say "none", so a limit of 0 or less is enforced when the form is
	 * published. A value is bounded by the body limit the transport applies,
	 * as in PHP, which has no per-field limit. */
	const mp_config_t config = {
		.max_fields = PG(max_input_vars) > 0 ? (size_t)PG(max_input_vars) : 1,
		.max_field_size = SIZE_MAX,
	};
	mp_processor_t *const processor = mp_processor_create(terminated, &config);

	if (processor != NULL) {
		processor->log_state = log_state;
	}

	return processor;
}

/* Refuses the form: the getter throws HttpException with `status`, which
 * answers the request when the handler does not catch it, and every later
 * getter throws the same. The body stays readable. */
static bool http_request_form_refuse(http_request_t *req, const int status, const char *reason)
{
	req->form_refused_status = (uint16_t)status;
	zend_throw_exception_ex(http_exception_ce, status, "the request form was refused: %s", reason);
	return false;
}

static const char *http_request_form_refusal_reason(const mp_processor_t *processor)
{
	switch (processor->refusal) {
		case MP_REFUSAL_TOO_MANY_FIELDS:
			return "more fields than max_input_vars";
		default:
			return "the multipart body is malformed";
	}
}

static bool http_request_form_decode_urlencoded(http_request_t *req, zval *post)
{
	if (req->body == NULL || ZSTR_LEN(req->body) == 0) {
		return true;
	}

	switch (http_form_vars_decode(Z_ARRVAL_P(post), ZSTR_VAL(req->body), ZSTR_LEN(req->body), "&",
								  PG(max_input_vars), PG(max_input_nesting_level))) {
		case HTTP_FORM_VARS_TOO_MANY:
			return http_request_form_refuse(req, 400, "more fields than max_input_vars");
		case HTTP_FORM_VARS_TOO_DEEP:
			return http_request_form_refuse(req, 400,
											"a name nested deeper than max_input_nesting_level");
		default:
			return true;
	}
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

static void http_request_form_discard(mp_processor_t *processor)
{
	mp_processor_cleanup_temp_files(processor);
	mp_processor_destroy(processor);
}

/* Parses a buffered body into req->multipart_proc. False with an exception
 * when the body is refused or a yield ended in one; nothing is kept then. */
static bool http_request_form_parse_buffered(http_request_t *req)
{
	mp_processor_t *const parsing = http_request_form_open_multipart(req, NULL);

	if (parsing == NULL) {
		return http_request_form_refuse(req, 400, "a multipart body without a usable boundary");
	}

	const char *next = req->body != NULL ? ZSTR_VAL(req->body) : "";
	size_t remaining = req->body != NULL ? ZSTR_LEN(req->body) : 0;

	while (remaining > 0) {
		const size_t slice =
			remaining < HTTP_REQUEST_FORM_FEED_SLICE ? remaining : HTTP_REQUEST_FORM_FEED_SLICE;

		if (mp_processor_feed(parsing, next, slice) < 0) {
			const char *const reason = http_request_form_refusal_reason(parsing);

			http_request_form_discard(parsing);
			return http_request_form_refuse(req, 400, reason);
		}

		next += slice;
		remaining -= slice;

		if (remaining > 0 && !http_request_form_yield()) {
			http_request_form_discard(parsing);
			return false;
		}
	}

	req->multipart_proc = parsing;
	return true;
}

static bool http_request_form_publish(http_request_t *req, zval *post, zval *files)
{
	const mp_processor_t *const processor = req->multipart_proc;
	size_t field_count;
	const mp_field_info_t *const fields = mp_processor_get_fields(processor, &field_count);
	bool too_deep = false;

	if ((zend_long)field_count > PG(max_input_vars)) {
		return http_request_form_refuse(req, 400, "more fields than max_input_vars");
	}

	for (size_t i = 0; i < field_count; i++) {
		if (fields[i].name == NULL) {
			continue;
		}

		char *value = estrndup(fields[i].value, fields[i].value_len);
		size_t filtered_len;

		if (http_form_vars_filter(fields[i].name, &value, fields[i].value_len, &filtered_len)) {
			zval stored;

			ZVAL_STRINGL_FAST(&stored, value, filtered_len);
			too_deep |= http_form_vars_register(Z_ARRVAL_P(post), fields[i].name,
												strlen(fields[i].name), &stored,
												PG(max_input_nesting_level))
						== HTTP_FORM_VARS_TOO_DEEP;
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

		too_deep |= http_form_vars_register(Z_ARRVAL_P(files), uploads[i].field_name,
											strlen(uploads[i].field_name), upload,
											PG(max_input_nesting_level))
					== HTTP_FORM_VARS_TOO_DEEP;
		efree(upload);
	}

	if (too_deep) {
		return http_request_form_refuse(req, 400,
										"a name nested deeper than max_input_nesting_level");
	}

	return true;
}

static bool http_request_form_decode_multipart(http_request_t *req, zval *post, zval *files)
{
	if (req->multipart_proc == NULL && !http_request_form_parse_buffered(req)) {
		return false;
	}

	return http_request_form_publish(req, post, files);
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

	if (req->form_refused_status != 0) {
		zend_throw_exception_ex(http_exception_ce, req->form_refused_status,
								"the request form was refused");
		return false;
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
		built = http_request_form_decode_urlencoded(req, &post);
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
