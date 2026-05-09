/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "php.h"
#include "http_rfc5987.h"

static const char hex_upper[] = "0123456789ABCDEF";

static inline bool is_unreserved_or_attr(unsigned char c)
{
	/* RFC 3986 unreserved + RFC 5987 attr-char (`!#$&+-.^_`|~`).
	 * Conservative: anything outside this set is percent-encoded. */
	if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
		return true;
	}
	switch (c) {
	case '-':
	case '.':
	case '_':
	case '~':
		return true;
	default:
		return false;
	}
}

void http_rfc5987_encode(smart_str *out, const char *src, size_t src_len)
{
	for (size_t i = 0; i < src_len; i++) {
		const unsigned char c = (unsigned char)src[i];
		if (is_unreserved_or_attr(c)) {
			smart_str_appendc(out, (char)c);
		} else {
			smart_str_appendc(out, '%');
			smart_str_appendc(out, hex_upper[c >> 4]);
			smart_str_appendc(out, hex_upper[c & 0xf]);
		}
	}
}

static inline int hex_digit(char c)
{
	if (c >= '0' && c <= '9') {
		return c - '0';
	}
	if (c >= 'a' && c <= 'f') {
		return c - 'a' + 10;
	}
	if (c >= 'A' && c <= 'F') {
		return c - 'A' + 10;
	}
	return -1;
}

size_t http_rfc5987_decode(char *out, const char *src, size_t src_len)
{
	size_t w = 0;
	for (size_t i = 0; i < src_len; i++) {
		const char c = src[i];
		if (c == '%' && i + 2 < src_len) {
			const int hi = hex_digit(src[i + 1]);
			const int lo = hex_digit(src[i + 2]);
			if (hi >= 0 && lo >= 0) {
				out[w++] = (char)((hi << 4) | lo);
				i += 2;
				continue;
			}
		}
		out[w++] = c;
	}
	return w;
}
