/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

/* Weak-ETag computation + If-None-Match comparison. */

#ifndef TRUE_ASYNC_HTTP_ETAG_H
#define TRUE_ASYNC_HTTP_ETAG_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <sys/stat.h>

/* W/"<16 hex>" — 20 chars. */
#define HTTP_ETAG_LEN 20
#define HTTP_ETAG_BUF_LEN (HTTP_ETAG_LEN + 1)

void http_etag_format_strong(const struct stat *st, char buf[HTTP_ETAG_BUF_LEN]);

/* RFC 9110 §13.1.2 weak-equal comparison. `header` is the raw
 * If-None-Match value; `etag` is the canonical W/"..." form written
 * by http_etag_format_strong. */
bool http_etag_match_inm(const char *header, size_t header_len, const char *etag, size_t etag_len);

#endif
