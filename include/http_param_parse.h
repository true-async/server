/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

/* Iterator over `;`-separated `name=value` parameters in an HTTP
 * header value (Content-Type, Content-Disposition, Cache-Control, ...).
 *
 * Caller is responsible for skipping the type token (e.g. "form-data",
 * "text/plain") before the first call — the iterator only walks the
 * parameter tail. Both `name` and `value` are returned as views into
 * the source buffer; quoted values come back without surrounding
 * quotes but with internal `\\`-escapes left intact (current callers
 * don't need RFC 7230 quoted-pair dequoting). */

#ifndef TRUE_ASYNC_HTTP_PARAM_PARSE_H
#define TRUE_ASYNC_HTTP_PARAM_PARSE_H

#include <stdbool.h>
#include <stddef.h>

typedef struct
{
	const char *name;
	size_t name_len;
	const char *value;
	size_t value_len;
	bool quoted;
} http_param_t;

/* Advance `*cursor` past one `; name=value` parameter; on success fill
 * `*out` with views into the underlying buffer and return true. Return
 * false when no more parameters remain (`*cursor` may point at NUL or
 * past `end`). Pass `end == NULL` for NUL-terminated input. */
bool http_header_param_next(const char **cursor, const char *end, http_param_t *out);

#endif
