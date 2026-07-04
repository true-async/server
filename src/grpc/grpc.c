/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+

  gRPC wire-protocol helpers layered on the existing HTTP/2 stack: request
  classification, grpc-timeout parsing, and the 5-byte length-prefix
  framing codec. Protobuf stays in PHP userland — this file only moves
  opaque length-prefixed octets. See docs/PLAN_GRPC.md.
*/

#include "grpc.h"
#include "http1/http_parser.h"   /* http_request_t */

#include <string.h>
#include <strings.h>             /* strncasecmp */

#ifdef HAVE_HTTP_COMPRESSION
#  include "compression/http_compression_message.h"  /* one-shot gzip */
#endif

bool grpc_request_is_grpc(const http_request_t *req)
{
    if (req == NULL || req->headers == NULL) {
        return false;
    }

    zval *ct = zend_hash_str_find(req->headers, "content-type",
                                  sizeof("content-type") - 1);

    if (ct == NULL || Z_TYPE_P(ct) != IS_STRING) {
        return false;
    }

    const size_t prefix_len = sizeof(GRPC_CONTENT_TYPE) - 1;   /* 16 */

    return Z_STRLEN_P(ct) >= prefix_len
        && strncasecmp(Z_STRVAL_P(ct), GRPC_CONTENT_TYPE, prefix_len) == 0;
}

uint64_t grpc_parse_timeout_ns(const http_request_t *req)
{
    if (req == NULL || req->headers == NULL) {
        return 0;
    }

    zval *to = zend_hash_str_find(req->headers, "grpc-timeout",
                                  sizeof("grpc-timeout") - 1);

    if (to == NULL || Z_TYPE_P(to) != IS_STRING) {
        return 0;
    }

    const char  *s = Z_STRVAL_P(to);
    const size_t n = Z_STRLEN_P(to);

    /* Spec: 1..8 ASCII digits followed by a single unit char. */
    if (n < 2 || n > 9) {
        return 0;
    }

    uint64_t value = 0;
    for (size_t i = 0; i + 1 < n; i++) {
        if (s[i] < '0' || s[i] > '9') {
            return 0;
        }
        value = value * 10u + (uint64_t)(s[i] - '0');
    }

    uint64_t unit_ns;
    switch (s[n - 1]) {
        case 'H': unit_ns = 3600ULL * 1000000000ULL; break;   /* hours   */
        case 'M': unit_ns =   60ULL * 1000000000ULL; break;   /* minutes */
        case 'S': unit_ns =          1000000000ULL;  break;   /* seconds */
        case 'm': unit_ns =             1000000ULL;  break;   /* millis  */
        case 'u': unit_ns =                1000ULL;  break;   /* micros  */
        case 'n': unit_ns =                   1ULL;  break;   /* nanos   */
        default:  return 0;
    }

    return value * unit_ns;
}

zend_string *grpc_frame_message(const char *msg, size_t len, bool compressed)
{
    zend_string *out = zend_string_alloc(5 + len, 0);
    unsigned char *p  = (unsigned char *)ZSTR_VAL(out);

    p[0] = compressed ? 1u : 0u;
    p[1] = (unsigned char)((len >> 24) & 0xffu);
    p[2] = (unsigned char)((len >> 16) & 0xffu);
    p[3] = (unsigned char)((len >>  8) & 0xffu);
    p[4] = (unsigned char)( len        & 0xffu);

    if (len > 0) {
        memcpy(p + 5, msg, len);
    }

    ZSTR_VAL(out)[5 + len] = '\0';
    return out;
}

int grpc_deframe_next(const char *buf, size_t len, size_t *cursor,
                      uint32_t max_msg, bool *out_compressed,
                      zend_string **out)
{
    const size_t pos = *cursor;

    /* Need the full 5-byte frame header before we can size the message. */
    if (pos + 5 > len) {
        return 0;
    }

    const unsigned char *h = (const unsigned char *)buf + pos;

    const uint32_t mlen = ((uint32_t)h[1] << 24)
                        | ((uint32_t)h[2] << 16)
                        | ((uint32_t)h[3] <<  8)
                        |  (uint32_t)h[4];

    if (mlen > max_msg) {
        return -1;
    }

    /* Message body may span DATA frames — wait until it's all buffered. */
    if (pos + 5 + (size_t)mlen > len) {
        return 0;
    }

    if (out_compressed != NULL) {
        *out_compressed = (h[0] != 0);
    }

    *out    = zend_string_init(buf + pos + 5, mlen, 0);
    *cursor = pos + 5 + (size_t)mlen;
    return 1;
}

int grpc_message_inflate(const http_request_t *req,
                         const char *in, size_t in_len, zend_string **out)
{
#ifdef HAVE_HTTP_COMPRESSION
    /* Compressed flag set → the algorithm is named by grpc-encoding. gRPC's
     * baseline is gzip; anything else is unsupported here. */
    zval *enc = (req != NULL && req->headers != NULL)
        ? zend_hash_str_find(req->headers, "grpc-encoding",
                             sizeof("grpc-encoding") - 1)
        : NULL;

    if (enc == NULL || Z_TYPE_P(enc) != IS_STRING
        || !zend_string_equals_literal(Z_STR_P(enc), "gzip")) {
        return -1;
    }

    return http_compression_gzip_inflate_buffer(in, in_len,
                                                GRPC_MAX_RECV_MESSAGE, out) == 0
               ? 0 : -1;
#else
    (void)req; (void)in; (void)in_len; (void)out;
    return -1;
#endif
}

zend_string *grpc_message_deflate_gzip(const char *in, size_t in_len)
{
#ifdef HAVE_HTTP_COMPRESSION
    return http_compression_gzip_deflate_buffer(in, in_len, 6 /* default */);
#else
    (void)in; (void)in_len;
    return NULL;
#endif
}
