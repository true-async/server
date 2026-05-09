/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

/* RFC 9110 §14.1.2 single-range parser. Multi-range syntax is
 * recognised but reported as UNSUPPORTED; the caller falls back to
 * 200 with the full body (which RFC 9110 explicitly permits). */

#ifndef TRUE_ASYNC_HTTP_RANGE_H
#define TRUE_ASYNC_HTTP_RANGE_H

#include <stddef.h>
#include <stdint.h>

typedef enum
{
	HTTP_RANGE_ABSENT,			/* nil header — caller serves 200 */
	HTTP_RANGE_OK,				/* valid → 206 with [first,last] inclusive */
	HTTP_RANGE_NOT_SATISFIABLE, /* syntactically OK, range past EOF → 416 */
	HTTP_RANGE_UNSUPPORTED,		/* malformed / multi-range → caller serves 200 */
} http_range_result_t;

http_range_result_t http_range_parse(const char *header, size_t header_len, uint64_t content_length,
									 uint64_t *out_first, uint64_t *out_last);

#endif
