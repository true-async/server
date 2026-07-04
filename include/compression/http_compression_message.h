/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

/*
 * One-shot, whole-buffer gzip (RFC 1952) helpers that reuse the module's
 * existing zlib(-ng) backend. The streaming http_encoder_t vtable and the
 * request-body decoder both operate on the HTTP *body*; message-level
 * compression (e.g. gRPC per-message, where each framed message carries its
 * own compressed flag) needs to compress/decompress a single buffer at a
 * time, so it goes through these instead of duplicating a zlib wrapper.
 *
 * Only defined when a zlib backend is compiled in (HAVE_HTTP_COMPRESSION);
 * the deflate side lives in http_compression_gzip.c, the inflate side is
 * shared with the request-body decoder in http_compression_request.c.
 */
#ifndef HTTP_COMPRESSION_MESSAGE_H
#define HTTP_COMPRESSION_MESSAGE_H

#include "php.h"   /* zend_string */
#include <stddef.h>

/* Compress `in` as a standalone gzip member. Returns a new zend_string the
 * caller owns, or NULL on failure. `level` is clamped to 1..9. */
zend_string *http_compression_gzip_deflate_buffer(const char *in, size_t in_len,
                                                   int level);

/* Inflate a standalone gzip (or zlib) member into a fresh zend_string.
 * Returns 0 on success (*out set, caller owns it), -1 on a malformed stream,
 * and -2 when the output would exceed `max_out` (max_out == 0 → unbounded). */
int http_compression_gzip_inflate_buffer(const char *in, size_t in_len,
                                         size_t max_out, zend_string **out);

#endif /* HTTP_COMPRESSION_MESSAGE_H */
