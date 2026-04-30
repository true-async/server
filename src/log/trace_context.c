/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include "log/trace_context.h"
#include "http1/http_parser.h"           /* http_request_t */

#include <string.h>

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    /* W3C TC §3.2.2.1 mandates lower-case; we reject upper-case. */
    return -1;
}

static bool decode_hex(const char *src, size_t src_chars, uint8_t *dst)
{
    if ((src_chars & 1u) != 0) {
        return false;
    }
    for (size_t i = 0; i < src_chars; i += 2) {
        int hi = hex_nibble(src[i]);
        int lo = hex_nibble(src[i + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        dst[i / 2] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

static bool is_all_zero(const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (p[i] != 0) return false;
    }
    return true;
}

bool trace_parse_traceparent(const char *header, size_t len,
                             uint8_t out_trace_id[16],
                             uint8_t out_span_id[8],
                             uint8_t *out_flags)
{
    /* "00-<32>-<16>-<2>" = 2 + 1 + 32 + 1 + 16 + 1 + 2 = 55 chars. */
    if (header == NULL || len != 55) {
        return false;
    }
    if (header[2] != '-' || header[35] != '-' || header[52] != '-') {
        return false;
    }

    /* Version. Future versions are explicitly rejected here — a v01+
     * peer's header layout is not guaranteed compatible. */
    uint8_t version;
    if (!decode_hex(header, 2, &version) || version != 0x00) {
        return false;
    }

    uint8_t trace_id[16];
    uint8_t span_id[8];
    uint8_t flags;
    if (!decode_hex(header + 3,  32, trace_id)
        || !decode_hex(header + 36, 16, span_id)
        || !decode_hex(header + 53, 2,  &flags)) {
        return false;
    }

    /* Invalid sentinels (W3C TC §3.2.2.3 / §3.2.2.4). */
    if (is_all_zero(trace_id, 16) || is_all_zero(span_id, 8)) {
        return false;
    }

    memcpy(out_trace_id, trace_id, 16);
    memcpy(out_span_id, span_id, 8);
    *out_flags = flags;
    return true;
}

void trace_hex_encode(const uint8_t *src, size_t src_len, char *dst)
{
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < src_len; i++) {
        dst[2 * i]     = digits[(src[i] >> 4) & 0xf];
        dst[2 * i + 1] = digits[src[i]        & 0xf];
    }
    dst[2 * src_len] = '\0';
}

void http_request_parse_trace_context(http_request_t *req)
{
    if (req == NULL || req->headers == NULL) {
        return;
    }

    /* Header keys in req->headers are lowercase by convention. */
    zval *tp = zend_hash_str_find(req->headers,
                                  "traceparent", sizeof("traceparent") - 1);
    if (tp == NULL || Z_TYPE_P(tp) != IS_STRING) {
        return;
    }

    uint8_t trace_id[16];
    uint8_t span_id[8];
    uint8_t flags;
    if (!trace_parse_traceparent(Z_STRVAL_P(tp), Z_STRLEN_P(tp),
                                 trace_id, span_id, &flags)) {
        return;
    }

    memcpy(req->trace_id, trace_id, 16);
    memcpy(req->span_id,  span_id,  8);
    req->trace_flags = flags;
    req->has_trace   = true;
    req->traceparent_raw = zend_string_copy(Z_STR_P(tp));

    /* tracestate is optional; size limit per W3C is 512 bytes but we
     * don't enforce here — the spec leaves enforcement to the consumer. */
    zval *ts = zend_hash_str_find(req->headers,
                                  "tracestate", sizeof("tracestate") - 1);
    if (ts != NULL && Z_TYPE_P(ts) == IS_STRING) {
        req->tracestate_raw = zend_string_copy(Z_STR_P(ts));
    }
}
