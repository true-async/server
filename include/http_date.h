/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

/* RFC 7231 IMF-fixdate format/parse. Self-contained — no strftime
 * (locale-free), no strptime fallback for the format path. The parse
 * path additionally accepts the two legacy formats RFC 9110 §5.6.7
 * mandates (RFC 850 and asctime). */

#ifndef TRUE_ASYNC_HTTP_DATE_H
#define TRUE_ASYNC_HTTP_DATE_H

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

/* "Sun, 06 Nov 1994 08:49:37 GMT" — 29 chars + NUL. */
#define HTTP_DATE_LEN 29
#define HTTP_DATE_BUF_LEN (HTTP_DATE_LEN + 1)

void http_date_format_imf(time_t t, char buf[HTTP_DATE_BUF_LEN]);

/* Returns (time_t)-1 on parse error. */
time_t http_date_parse_imf(const char *src, size_t src_len);

#endif
