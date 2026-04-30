/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

#ifndef HTTP_LOG_TRACE_CONTEXT_H
#define HTTP_LOG_TRACE_CONTEXT_H

#include "php.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Parse a W3C Trace Context `traceparent` header value into raw bytes.
 *
 * Accepts only the version-00 format (RFC W3C TC §3.2.2.1):
 *   traceparent = "00-<32 lower-hex>-<16 lower-hex>-<2 lower-hex>"
 *
 * Validates: total length 55, dash positions, lower-hex chars, and
 * the W3C invalid sentinels (all-zero trace_id / span_id are rejected).
 *
 * Out parameters are written only on SUCCESS. Returns true if valid. */
bool trace_parse_traceparent(const char *header, size_t len,
                             uint8_t out_trace_id[16],
                             uint8_t out_span_id[8],
                             uint8_t *out_flags);

/* Lower-hex format helpers. `dst` must be at least 2*src_len + 1
 * bytes; the result is NUL-terminated. */
void trace_hex_encode(const uint8_t *src, size_t src_len,
                      char *dst);

/* Look up "traceparent"/"tracestate" in the parsed request headers
 * and populate req->trace_*, req->traceparent_raw, req->tracestate_raw.
 * No-op when telemetry is disabled or no valid traceparent is present. */
struct http_request_t;
void http_request_parse_trace_context(struct http_request_t *req);

#ifdef __cplusplus
}
#endif

#endif /* HTTP_LOG_TRACE_CONTEXT_H */
