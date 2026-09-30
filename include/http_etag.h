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
#include <time.h>
#include "Zend/zend_stream.h"  /* zend_stat_t — struct stat on POSIX, _stat64 on Windows */

/* W/"<16 hex>" — 20 chars. */
#define HTTP_ETAG_LEN 20
#define HTTP_ETAG_BUF_LEN (HTTP_ETAG_LEN + 1)

void http_etag_format_strong(const zend_stat_t *st, char buf[HTTP_ETAG_BUF_LEN]);

/* RFC 9110 §13.1.2 weak-equal comparison. `header` is the raw
 * If-None-Match value; `etag` is the canonical W/"..." form written
 * by http_etag_format_strong. */
bool http_etag_match_inm(const char *header, size_t header_len, const char *etag, size_t etag_len);

/* Returns true when the request's conditional-GET headers match the
 * resource state and the server SHOULD answer 304. The etag value,
 * when supplied, must be the canonical W/"..." form produced by
 * http_etag_format_strong. last_modified is in seconds. */
bool http_conditional_check(const char *if_none_match, size_t if_none_match_len,
							const char *if_modified_since, size_t if_modified_since_len,
							const char *etag, size_t etag_len, time_t last_modified);

#endif
