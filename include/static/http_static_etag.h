/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

/* Weak-ETag computation + conditional-GET evaluation. */

#ifndef TRUE_ASYNC_HTTP_STATIC_ETAG_H
#define TRUE_ASYNC_HTTP_STATIC_ETAG_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <sys/stat.h>

/* Buffer-size + NUL for a weak ETag in the form W/"<16 hex>" — that
 * is `W` + `/` + `"` + 16 hex + `"` = 20 chars, plus the NUL. */
#define HTTP_STATIC_ETAG_LEN 20
#define HTTP_STATIC_ETAG_BUF_LEN (HTTP_STATIC_ETAG_LEN + 1)

void http_static_etag_format(const struct stat *st, char buf[HTTP_STATIC_ETAG_BUF_LEN]);

/* IMF-fixdate "Sun, 06 Nov 1994 08:49:37 GMT" + NUL. */
#define HTTP_STATIC_DATE_BUF_LEN 30
#define HTTP_STATIC_DATE_LEN (HTTP_STATIC_DATE_BUF_LEN - 1)

void http_static_format_http_date(time_t t, char buf[HTTP_STATIC_DATE_BUF_LEN]);

/* Returns true when the request's conditional-GET headers
 * (If-None-Match / If-Modified-Since) match the resource state and
 * therefore the server SHOULD answer 304. The etag value, when
 * supplied, must be the same string written by http_static_etag_format
 * (W/"..." form). last_modified is in seconds (st_mtime). */
bool http_static_conditional_match(const char *if_none_match, size_t if_none_match_len,
								   const char *if_modified_since, size_t if_modified_since_len,
								   const char *etag, size_t etag_len, time_t last_modified);

#endif
