/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

/* Names go through PHP's own routines, php_default_treat_data for an
 * url-encoded body and php_register_variable_ex for multipart parts, so a
 * form reads exactly like $_POST: array notation, max_input_vars,
 * max_input_nesting_level, and `.` or space in a base name turned into `_`.
 * getQuery() parses the query string the same way. */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "php.h"
#include "main/php_variables.h"
#include "http1/http_parser.h"
#include "http_request_form.h"
#include "formats/form_content_type.h"
#include "formats/multipart_processor.h"

#include <string.h>

/* uploaded_file.c; an emalloc'd zval the caller frees after moving the object out. */
extern zval *uploaded_file_create_from_info(mp_file_info_t *info);

mp_processor_t *http_request_form_open_multipart(const http_request_t *req)
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

	char terminated[HTTP_FORM_BOUNDARY_MAX_LEN + 1];

	memcpy(terminated, boundary, boundary_len);
	terminated[boundary_len] = '\0';

	return mp_processor_create(terminated, NULL);
}

static void http_request_form_decode_urlencoded(const http_request_t *req, zval *post)
{
	if (req->body == NULL || ZSTR_LEN(req->body) == 0) {
		return;
	}

	/* php_default_treat_data takes the copy and frees it. */
	php_default_treat_data(PARSE_STRING, estrndup(ZSTR_VAL(req->body), ZSTR_LEN(req->body)), post);
}

static mp_processor_t *http_request_form_parse_buffered(const http_request_t *req)
{
	if (req->body == NULL) {
		return NULL;
	}

	mp_processor_t *const processor = http_request_form_open_multipart(req);

	if (processor == NULL) {
		return NULL;
	}

	if (mp_processor_feed(processor, ZSTR_VAL(req->body), ZSTR_LEN(req->body)) < 0) {
		mp_processor_cleanup_temp_files(processor);
		mp_processor_destroy(processor);
		return NULL;
	}

	return processor;
}

static void http_request_form_publish(const mp_processor_t *processor, zval *post, zval *files)
{
	size_t field_count;
	const mp_field_info_t *const fields = mp_processor_get_fields(processor, &field_count);

	for (size_t i = 0; i < field_count; i++) {
		if (fields[i].name != NULL) {
			php_register_variable_safe(fields[i].name, fields[i].value, fields[i].value_len, post);
		}
	}

	size_t upload_count;
	mp_file_info_t *const uploads = mp_processor_get_files(processor, &upload_count);

	for (size_t i = 0; i < upload_count; i++) {
		if (uploads[i].field_name == NULL) {
			continue;
		}

		zval *const upload = uploaded_file_create_from_info(&uploads[i]);

		if (upload == NULL) {
			continue;
		}

		php_register_variable_ex(uploads[i].field_name, upload, files);
		efree(upload);
	}
}

static void http_request_form_decode_multipart(http_request_t *req, zval *post, zval *files)
{
	if (req->multipart_proc == NULL) {
		req->multipart_proc = http_request_form_parse_buffered(req);
	}

	if (req->multipart_proc != NULL) {
		http_request_form_publish(req->multipart_proc, post, files);
	}
}

void http_request_form_build(http_request_t *req)
{
	if (req->form_built || !req->complete) {
		return;
	}

	zval post;
	zval files;

	array_init(&post);
	array_init(&files);

	if (req->form_kind == HTTP_FORM_URLENCODED) {
		http_request_form_decode_urlencoded(req, &post);
	} else if (req->form_kind == HTTP_FORM_MULTIPART) {
		http_request_form_decode_multipart(req, &post, &files);
	}

	req->post_data = Z_ARR(post);
	req->files = Z_ARR(files);
	req->form_built = true;
}
