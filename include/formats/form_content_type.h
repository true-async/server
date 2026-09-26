/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

/* Recognises an HTML form in a request's Content-Type value.
 *
 * The media type is the token before the first ';', compared as a whole and
 * case-insensitively (RFC 9110 §8.3.1): "multipart/form-data-x" is not a form.
 * Pure C over a byte range, so the unit suite runs it without a request. */

#ifndef TRUE_ASYNC_FORM_CONTENT_TYPE_H
#define TRUE_ASYNC_FORM_CONTENT_TYPE_H

#include <stdbool.h>
#include <stddef.h>

/* RFC 2046 §5.1.1: a boundary is 1 to 70 characters. */
#define HTTP_FORM_BOUNDARY_MAX_LEN 70

typedef enum
{
	HTTP_FORM_NONE = 0,
	HTTP_FORM_URLENCODED, /* application/x-www-form-urlencoded */
	HTTP_FORM_MULTIPART,  /* multipart/form-data */
} http_form_kind_t;

/* Which form, if any, a Content-Type value announces. NULL or empty is no form.
 * A multipart type is reported whether or not it carries a usable boundary;
 * http_form_find_boundary() says that. */
http_form_kind_t http_form_kind_of(const char *content_type, size_t content_type_len);

/* The boundary parameter of a multipart Content-Type value, as a view into
 * `content_type` with the quotes of a quoted value removed. The parameter name
 * matches case-insensitively and the first one wins. False when the parameter
 * is absent or its value is empty or over HTTP_FORM_BOUNDARY_MAX_LEN bytes;
 * `boundary` and `boundary_len` are left untouched then. */
bool http_form_find_boundary(const char *content_type, size_t content_type_len,
							 const char **boundary, size_t *boundary_len);

#endif
