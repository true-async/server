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

/* Format a weak ETag for the given (mtime_ns, size, ino) into `buf`.
 * Always writes exactly `HTTP_STATIC_ETAG_BUF_LEN` bytes (NUL-terminated)
 * — the leading `W/`, the opening quote, 16 hex chars, the closing
 * quote, NUL. */
#define HTTP_STATIC_ETAG_BUF_LEN 22  /* W/" + 16 hex + " + NUL */

void http_static_etag_format(struct stat *st, char buf[HTTP_STATIC_ETAG_BUF_LEN]);

/* Format an HTTP-date for the Last-Modified / Date header. Writes
 * exactly 30 bytes (incl. NUL): "Sun, 06 Nov 1994 08:49:37 GMT". */
#define HTTP_STATIC_DATE_BUF_LEN 30

void http_static_format_http_date(time_t t, char buf[HTTP_STATIC_DATE_BUF_LEN]);

/* Returns true when the request's conditional-GET headers
 * (If-None-Match / If-Modified-Since) match the resource state and
 * therefore the server SHOULD answer 304. The etag value, when
 * supplied, must be the same string written by http_static_etag_format
 * (W/"..." form). last_modified is in seconds (st_mtime). */
bool http_static_conditional_match(const char *if_none_match,
                                   size_t if_none_match_len,
                                   const char *if_modified_since,
                                   size_t if_modified_since_len,
                                   const char *etag, size_t etag_len,
                                   time_t last_modified);

#endif
