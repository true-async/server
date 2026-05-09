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
#include "http_range.h"

#include <string.h>

/* RFC 9110 §14.1.2.
 *
 * Accepts:
 *   bytes=A-B      first..last (inclusive)
 *   bytes=A-       first..size-1
 *   bytes=-N       size-N..size-1 (suffix-length form)
 *
 * Multi-range syntax (comma-separated) is recognised but reported
 * UNSUPPORTED — multipart/byteranges responses are a separate
 * follow-up. The caller may serve 200 with the full body
 * ("a server MAY ignore the Range header field"). */
http_range_result_t http_range_parse(const char *header, size_t header_len, uint64_t content_length,
									 uint64_t *out_first, uint64_t *out_last)
{
	if (header == NULL || header_len == 0) {
		return HTTP_RANGE_ABSENT;
	}
	if (header_len < 7) {
		return HTTP_RANGE_UNSUPPORTED;
	}
	if (memcmp(header, "bytes=", 6) != 0) {
		return HTTP_RANGE_UNSUPPORTED;
	}
	const char *p = header + 6;
	const char *end = header + header_len;
	while (p < end && (*p == ' ' || *p == '\t')) {
		p++;
	}
	/* Reject multi-range: scan for ',' before the dash. */
	for (const char *q = p; q < end; q++) {
		if (*q == ',') {
			return HTTP_RANGE_UNSUPPORTED;
		}
	}
	if (p >= end) {
		return HTTP_RANGE_UNSUPPORTED;
	}

	bool suffix_form = false;
	uint64_t first = 0;
	bool first_set = false;
	if (*p == '-') {
		suffix_form = true;
		p++;
	} else {
		while (p < end && *p >= '0' && *p <= '9') {
			if (first > UINT64_MAX / 10) {
				return HTTP_RANGE_UNSUPPORTED;
			}
			first = first * 10 + (uint64_t)(*p - '0');
			first_set = true;
			p++;
		}
		if (!first_set || p >= end || *p != '-') {
			return HTTP_RANGE_UNSUPPORTED;
		}
		p++;
	}

	uint64_t last = 0;
	bool last_set = false;
	while (p < end && *p >= '0' && *p <= '9') {
		if (last > UINT64_MAX / 10) {
			return HTTP_RANGE_UNSUPPORTED;
		}
		last = last * 10 + (uint64_t)(*p - '0');
		last_set = true;
		p++;
	}
	while (p < end && (*p == ' ' || *p == '\t')) {
		p++;
	}
	if (p != end) {
		return HTTP_RANGE_UNSUPPORTED;
	}

	if (content_length == 0) {
		return HTTP_RANGE_NOT_SATISFIABLE;
	}

	if (suffix_form) {
		if (!last_set || last == 0) {
			return HTTP_RANGE_UNSUPPORTED;
		}
		if (last >= content_length) {
			/* "last N where N >= size" → whole file (RFC 9110: a
			 * suffix-length larger than the resource length is
			 * treated as the whole resource, status still 206). */
			*out_first = 0;
		} else {
			*out_first = content_length - last;
		}
		*out_last = content_length - 1;
		return HTTP_RANGE_OK;
	}
	if (first >= content_length) {
		return HTTP_RANGE_NOT_SATISFIABLE;
	}
	if (!last_set) {
		*out_first = first;
		*out_last = content_length - 1;
		return HTTP_RANGE_OK;
	}
	if (last < first) {
		return HTTP_RANGE_UNSUPPORTED;
	}
	if (last >= content_length) {
		last = content_length - 1;
	}
	*out_first = first;
	*out_last = last;
	return HTTP_RANGE_OK;
}
