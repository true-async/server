/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

/* strncasecmp is not used: without php.h it is absent on MSVC, and this unit
 * is kept free of PHP so the unit suite can link it alone. */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "formats/form_content_type.h"
#include "http_param_parse.h"

#include <string.h>

#define MEDIA_TYPE_URLENCODED "application/x-www-form-urlencoded"
#define MEDIA_TYPE_MULTIPART "multipart/form-data"
#define PARAM_BOUNDARY "boundary"

static inline bool is_ows(const char c)
{
	return c == ' ' || c == '\t';
}

static inline char ascii_lower(const char c)
{
	return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
}

/* `literal` is lower-case. */
static bool equals_ignoring_case(const char *text, const size_t text_len, const char *literal,
								 const size_t literal_len)
{
	if (text_len != literal_len) {
		return false;
	}

	for (size_t i = 0; i < text_len; i++) {
		if (ascii_lower(text[i]) != literal[i]) {
			return false;
		}
	}

	return true;
}

http_form_kind_t http_form_kind_of(const char *content_type, const size_t content_type_len)
{
	if (content_type == NULL || content_type_len == 0) {
		return HTTP_FORM_NONE;
	}

	const char *const semicolon = memchr(content_type, ';', content_type_len);
	const char *type_start = content_type;
	const char *type_end = semicolon != NULL ? semicolon : content_type + content_type_len;

	while (type_start < type_end && is_ows(*type_start)) {
		type_start++;
	}

	while (type_end > type_start && is_ows(type_end[-1])) {
		type_end--;
	}

	const size_t type_len = (size_t)(type_end - type_start);

	if (equals_ignoring_case(type_start, type_len, MEDIA_TYPE_URLENCODED,
							 sizeof(MEDIA_TYPE_URLENCODED) - 1)) {
		return HTTP_FORM_URLENCODED;
	}

	if (equals_ignoring_case(type_start, type_len, MEDIA_TYPE_MULTIPART,
							 sizeof(MEDIA_TYPE_MULTIPART) - 1)) {
		return HTTP_FORM_MULTIPART;
	}

	return HTTP_FORM_NONE;
}

bool http_form_find_boundary(const char *content_type, const size_t content_type_len,
							 const char **boundary, size_t *boundary_len)
{
	if (content_type == NULL) {
		return false;
	}

	const char *const end = content_type + content_type_len;
	const char *cursor = memchr(content_type, ';', content_type_len);
	http_param_t param;

	if (cursor == NULL) {
		return false;
	}

	while (http_header_param_next(&cursor, end, &param)) {
		if (!equals_ignoring_case(param.name, param.name_len, PARAM_BOUNDARY,
								  sizeof(PARAM_BOUNDARY) - 1)) {
			continue;
		}

		if (param.value_len == 0 || param.value_len > HTTP_FORM_BOUNDARY_MAX_LEN) {
			return false;
		}

		*boundary = param.value;
		*boundary_len = param.value_len;
		return true;
	}

	return false;
}
