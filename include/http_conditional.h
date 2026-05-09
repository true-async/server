/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

/* Conditional-GET evaluation (If-None-Match / If-Modified-Since →
 * 304). Single point of decision, callable from any module that
 * delivers a representation. */

#ifndef TRUE_ASYNC_HTTP_CONDITIONAL_H
#define TRUE_ASYNC_HTTP_CONDITIONAL_H

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

/* Returns true when the request's conditional-GET headers match the
 * resource state and the server SHOULD answer 304. The etag value,
 * when supplied, must be the canonical W/"..." form produced by
 * http_etag_format_strong. last_modified is in seconds. */
bool http_conditional_check(const char *if_none_match, size_t if_none_match_len,
							const char *if_modified_since, size_t if_modified_since_len,
							const char *etag, size_t etag_len, time_t last_modified);

#endif
