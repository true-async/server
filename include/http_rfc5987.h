/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

/* RFC 5987 §3.2 ext-value:
 *      charset "'" [ language ] "'" value-chars
 *
 * Used in HTTP header parameters that need to carry non-ASCII bytes,
 * notably Content-Disposition `filename*=UTF-8''...`. The encoder
 * percent-encodes everything outside RFC 3986 unreserved + RFC 5987
 * attr-char; the decoder accepts any percent-encoding (it does not
 * validate the charset prefix — that is the caller's job). */

#ifndef TRUE_ASYNC_HTTP_RFC5987_H
#define TRUE_ASYNC_HTTP_RFC5987_H

#include "zend_smart_str.h"
#include <stddef.h>

/* Append the percent-encoded `value-chars` of `src` onto `out`. The
 * caller is expected to have already written the "charset''" prefix. */
void http_rfc5987_encode(smart_str *out, const char *src, size_t src_len);

/* Decode the percent-encoded `value-chars` from `src` (length
 * `src_len`) into `out` (capacity must be at least `src_len`). Invalid
 * %XX sequences are passed through verbatim. Returns the number of
 * bytes written into `out`. */
size_t http_rfc5987_decode(char *out, const char *src, size_t src_len);

#endif
