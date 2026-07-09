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
#include "http1/http_parser.h"
#include "zend_smart_str.h"
#include "ext/standard/base64.h"
#include "core/http_protocol_handlers.h"

#include <string.h>
#include <strings.h>

#ifdef HAVE_HTTP_COMPRESSION
#  include "compression/http_compression_request.h"
#endif

grpc_mode_t grpc_request_mode(const http_request_t *req)
{
    if (req->headers == NULL) {
        return GRPC_MODE_NONE;
    }

    zval *content_type = zend_hash_str_find(req->headers, "content-type",
                                            sizeof("content-type") - 1);

    if (content_type == NULL || Z_TYPE_P(content_type) != IS_STRING) {
        return GRPC_MODE_NONE;
    }

    const char *val = Z_STRVAL_P(content_type);
    size_t      len = Z_STRLEN_P(content_type);

    const size_t grpc_len = sizeof(GRPC_CONTENT_TYPE) - 1;

    if (len < grpc_len || strncasecmp(val, GRPC_CONTENT_TYPE, grpc_len) != 0) {
        return GRPC_MODE_NONE;
    }

    /* "-web" is a prefix of "-web-text" — check web-text first */
    val += grpc_len;
    len -= grpc_len;

    const size_t text_len = sizeof(GRPC_WEB_TEXT_SUFFIX) - 1;
    const size_t web_len  = sizeof(GRPC_WEB_SUFFIX) - 1;

    if (len >= text_len && strncasecmp(val, GRPC_WEB_TEXT_SUFFIX, text_len) == 0) {
        return GRPC_MODE_WEB_TEXT;
    }

    if (len >= web_len && strncasecmp(val, GRPC_WEB_SUFFIX, web_len) == 0) {
        return GRPC_MODE_WEB;
    }

    return GRPC_MODE_NATIVE;
}

grpc_mode_t grpc_classify(const http_request_t *req, HashTable *handlers)
{
    /* mode was stamped at headers-complete; here we only gate on a handler */
    const grpc_mode_t mode = (grpc_mode_t)req->grpc_mode;

    if (mode == GRPC_MODE_NONE
        || !http_protocol_has_handler(handlers, HTTP_PROTOCOL_GRPC)) {
        return GRPC_MODE_NONE;
    }

    return mode;
}

zend_string *grpc_web_text_encode(const char *in, const size_t len)
{
    return php_base64_encode((const unsigned char *)in, len);
}

static bool b64_decode_append(smart_str *out, const char *in, size_t len)
{
    zend_string *part = php_base64_decode_ex((const unsigned char *)in, len,
                                             /*strict=*/false);
    if (part == NULL) {
        return false;
    }

    smart_str_append(out, part);
    zend_string_release(part);
    return true;
}

zend_string *grpc_web_text_decode(const char *in, const size_t len)
{
    /* The body concatenates independently base64-encoded frames, each with
     * its own '='-padding; PHP's decoder does not reset at padding, so a
     * single pass garbles the tail — decode block-wise at each padding run. */
    smart_str out = {0};
    size_t    start = 0;

    smart_str_alloc(&out, (len / 4) * 3 + 3, 0);

    for (size_t i = 0; i < len; i++) {
        if (in[i] != '=') {
            continue;
        }

        size_t end = i + 1;

        while (end < len && in[end] == '=') {
            end++;
        }

        if (!b64_decode_append(&out, in + start, end - start)) {
            smart_str_free(&out);
            return NULL;
        }

        start = end;
        i     = end - 1;
    }

    if (start < len && !b64_decode_append(&out, in + start, len - start)) {
        smart_str_free(&out);
        return NULL;
    }

    smart_str_0(&out);
    return smart_str_extract(&out);
}

/* 5-byte frame prefix: flag byte + uint32 big-endian payload length. */
static void grpc_write_frame_header(unsigned char *p, unsigned char flag,
                                    size_t len)
{
    p[0] = flag;
    p[1] = (unsigned char)((len >> 24) & 0xffu);
    p[2] = (unsigned char)((len >> 16) & 0xffu);
    p[3] = (unsigned char)((len >>  8) & 0xffu);
    p[4] = (unsigned char)( len        & 0xffu);
}

zend_string *grpc_web_trailer_frame(HashTable *trailers)
{
    zend_string *name;
    zval        *val;
    size_t       tlen = 0;

    if (trailers != NULL) {
        ZEND_HASH_FOREACH_STR_KEY_VAL(trailers, name, val) {
            if (name == NULL || Z_TYPE_P(val) != IS_STRING) { continue; }
            tlen += ZSTR_LEN(name) + 2 + Z_STRLEN_P(val) + 2;  /* ": " + CRLF */
        } ZEND_HASH_FOREACH_END();
    }

    zend_string *out = zend_string_alloc(5 + tlen, 0);
    char        *w   = ZSTR_VAL(out);

    grpc_write_frame_header((unsigned char *)w, 0x80u /* trailer frame */, tlen);
    w += 5;

    if (trailers != NULL) {
        ZEND_HASH_FOREACH_STR_KEY_VAL(trailers, name, val) {
            if (name == NULL || Z_TYPE_P(val) != IS_STRING) { continue; }
            memcpy(w, ZSTR_VAL(name), ZSTR_LEN(name)); w += ZSTR_LEN(name);
            memcpy(w, ": ", 2);                        w += 2;
            memcpy(w, Z_STRVAL_P(val), Z_STRLEN_P(val)); w += Z_STRLEN_P(val);
            memcpy(w, "\r\n", 2);                      w += 2;
        } ZEND_HASH_FOREACH_END();
    }

    *w = '\0';
    return out;
}

uint64_t grpc_parse_timeout_ns(const http_request_t *req)
{
    if (req == NULL || req->headers == NULL) {
        return 0;
    }

    zval *timeout = zend_hash_str_find(req->headers, "grpc-timeout",
                                       sizeof("grpc-timeout") - 1);

    if (timeout == NULL || Z_TYPE_P(timeout) != IS_STRING) {
        return 0;
    }

    const char  *s = Z_STRVAL_P(timeout);
    const size_t n = Z_STRLEN_P(timeout);

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
        case 'H': unit_ns = 3600ULL * 1000000000ULL; break;
        case 'M': unit_ns =   60ULL * 1000000000ULL; break;
        case 'S': unit_ns =          1000000000ULL;  break;
        case 'm': unit_ns =             1000000ULL;  break;
        case 'u': unit_ns =                1000ULL;  break;
        case 'n': unit_ns =                   1ULL;  break;
        default:  return 0;
    }

    /* 8 digits × the hour factor exceeds uint64 — clamp instead of wrapping
     * to a bogus small deadline. */
    if (value > UINT64_MAX / unit_ns) {
        return UINT64_MAX;
    }

    return value * unit_ns;
}

zend_string *grpc_frame_message(const char *msg, size_t len, bool compressed)
{
    zend_string *out = zend_string_alloc(5 + len, 0);

    grpc_write_frame_header((unsigned char *)ZSTR_VAL(out),
                            compressed ? 1u : 0u, len);

    if (len > 0) {
        memcpy(ZSTR_VAL(out) + 5, msg, len);
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

#ifdef HAVE_HTTP_COMPRESSION

int grpc_message_inflate(const http_request_t *req,
                         const char *in, size_t in_len, zend_string **out)
{
    zval *enc = (req->headers != NULL)
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
}

zend_string *grpc_message_deflate_gzip(const char *in, size_t in_len)
{
    return http_compression_gzip_deflate_buffer(in, in_len, 6 /* default */);
}

#endif /* HAVE_HTTP_COMPRESSION */
