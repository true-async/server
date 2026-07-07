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
#include "zend_smart_str.h"
#include "ext/standard/base64.h" /* grpc-web-text per-frame transform */
#include "core/http_protocol_handlers.h" /* handler-registry gate (grpc_classify) */

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

bool grpc_request_is_grpc_web(const http_request_t *req)
{
    if (req == NULL || req->headers == NULL) {
        return false;
    }

    zval *ct = zend_hash_str_find(req->headers, "content-type",
                                  sizeof("content-type") - 1);

    if (ct == NULL || Z_TYPE_P(ct) != IS_STRING) {
        return false;
    }

    const size_t prefix_len = sizeof(GRPC_WEB_CONTENT_TYPE_PREFIX) - 1;  /* 20 */

    return Z_STRLEN_P(ct) >= prefix_len
        && strncasecmp(Z_STRVAL_P(ct), GRPC_WEB_CONTENT_TYPE_PREFIX,
                       prefix_len) == 0;
}

bool grpc_request_is_grpc_web_text(const http_request_t *req)
{
    if (req == NULL || req->headers == NULL) {
        return false;
    }

    zval *ct = zend_hash_str_find(req->headers, "content-type",
                                  sizeof("content-type") - 1);

    if (ct == NULL || Z_TYPE_P(ct) != IS_STRING) {
        return false;
    }

    const size_t prefix_len = sizeof(GRPC_WEB_TEXT_CONTENT_TYPE_PREFIX) - 1;

    return Z_STRLEN_P(ct) >= prefix_len
        && strncasecmp(Z_STRVAL_P(ct), GRPC_WEB_TEXT_CONTENT_TYPE_PREFIX,
                       prefix_len) == 0;
}

grpc_mode_t grpc_classify(const http_request_t *req, HashTable *handlers)
{
    const grpc_mode_t mode = grpc_request_mode(req);

    if (mode == GRPC_MODE_NONE
        || !http_protocol_has_handler(handlers, HTTP_PROTOCOL_GRPC)) {
        return GRPC_MODE_NONE;
    }

    return mode;
}

grpc_mode_t grpc_request_mode(const http_request_t *req)
{
    if (!grpc_request_is_grpc(req)) {
        return GRPC_MODE_NONE;
    }

    /* Most-specific prefix first: is_grpc_web matches web-text too. */
    if (grpc_request_is_grpc_web_text(req)) {
        return GRPC_MODE_WEB_TEXT;
    }

    if (grpc_request_is_grpc_web(req)) {
        return GRPC_MODE_WEB;
    }

    return GRPC_MODE_NATIVE;
}

zend_string *grpc_web_text_encode(const char *in, const size_t len)
{
    return php_base64_encode((const unsigned char *)in, len);
}

zend_string *grpc_web_text_decode(const char *in, const size_t len)
{
    /* The body is a CONCATENATION of independently base64-encoded frames,
     * each with its own '='-padding (that is what grpc-web clients and our
     * own encoder emit). PHP's non-strict decoder does not reset its 6-bit
     * group at padding, so a single pass garbles everything after the first
     * block whose byte length is not a multiple of 3 — decode block-wise,
     * splitting after each padding run. Blocks that need no padding
     * (len % 3 == 0) leave the bit stream 4-char aligned, so letting them
     * merge into the next block is correct. */
    smart_str out = {0};
    size_t    start = 0;

    for (size_t i = 0; i < len; i++) {
        if (in[i] != '=') {
            continue;
        }

        size_t end = i + 1;

        while (end < len && in[end] == '=') {
            end++;
        }

        zend_string *const part = php_base64_decode_ex(
            (const unsigned char *)in + start, end - start, /*strict=*/false);

        if (part == NULL) {
            smart_str_free(&out);
            return NULL;
        }

        smart_str_append(&out, part);
        zend_string_release(part);
        start = end;
        i     = end - 1;
    }

    if (start < len) {
        zend_string *const part = php_base64_decode_ex(
            (const unsigned char *)in + start, len - start, /*strict=*/false);

        if (part == NULL) {
            smart_str_free(&out);
            return NULL;
        }

        smart_str_append(&out, part);
        zend_string_release(part);
    }

    if (out.s == NULL) {
        return ZSTR_EMPTY_ALLOC();
    }

    smart_str_0(&out);
    return smart_str_extract(&out);
}

zend_string *grpc_web_trailer_frame(HashTable *trailers)
{
    smart_str body = {0};

    if (trailers != NULL) {
        zend_string *name;
        zval        *val;
        ZEND_HASH_FOREACH_STR_KEY_VAL(trailers, name, val) {
            if (name == NULL || Z_TYPE_P(val) != IS_STRING) { continue; }
            smart_str_append(&body, name);
            smart_str_appendl(&body, ": ", 2);
            smart_str_append(&body, Z_STR_P(val));
            smart_str_appendl(&body, "\r\n", 2);
        } ZEND_HASH_FOREACH_END();
    }

    const size_t tlen = body.s != NULL ? ZSTR_LEN(body.s) : 0;

    zend_string   *out = zend_string_alloc(5 + tlen, 0);
    unsigned char *p   = (unsigned char *)ZSTR_VAL(out);

    p[0] = 0x80u;   /* trailer frame, uncompressed */
    p[1] = (unsigned char)((tlen >> 24) & 0xffu);
    p[2] = (unsigned char)((tlen >> 16) & 0xffu);
    p[3] = (unsigned char)((tlen >>  8) & 0xffu);
    p[4] = (unsigned char)( tlen        & 0xffu);

    if (tlen > 0) {
        memcpy(p + 5, ZSTR_VAL(body.s), tlen);
    }

    ZSTR_VAL(out)[5 + tlen] = '\0';
    smart_str_free(&body);
    return out;
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
