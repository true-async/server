/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

#ifndef HTTP2_SESSION_H
#define HTTP2_SESSION_H

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#ifdef HAVE_HTTP2

#include "php.h"
#include <nghttp2/nghttp2.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>     /* ssize_t */

/* Forward declarations. Kept out of the connection layer. */
typedef struct _http_connection_t http_connection_t;
typedef struct http2_session_t http2_session_t;
typedef struct http2_stream_t http2_stream_t;

/* Forward-declare http_request_t without pulling the HTTP/1 parser
 * header into this TU — matches the strategy.h pattern. */
struct http_request_t;

/* Dispatch callback fired by the HEADERS + END_HEADERS path for each
 * new stream (plan §3.6 — dispatch at HEADERS, not END_STREAM, so
 * gRPC bidi handlers can start executing before the body arrives).
 *
 * The strategy layer installs a trampoline that forwards to
 * http_protocol_strategy_t::on_request_ready; tests install their
 * own capture to inspect the request without a real connection. */
typedef void (*http2_request_ready_cb_t)(struct http_request_t *request,
                                         uint32_t stream_id,
                                         void *user_data);

#define HTTP2_SEND_IOV_MAX      16
/* Stack scratch for HEADERS/trailer submit. Sized so that a typical
 * response (auto-headers + ~50-70 user headers) fits without the
 * emalloc fallback — the fallback path exists but is meant to be rare.
 * 96 * max(sizeof(nghttp2_nv), sizeof(http2_header_view_t)) ≈ 4 KiB on
 * stack, well within coroutine frame budget. */
#define HTTP2_NV_SCRATCH        96

/* Initial SETTINGS table. Values chosen for REST/gRPC/upload
 * workloads: push off, 100 concurrent streams, 1 MiB initial window,
 * 64 KiB header list cap (matches HTTP_MAX_HEADERS_TOTAL), RFC-min
 * frame size. Exposed so unit tests can cross-check each value
 * against bytes on the wire. */
#define HTTP2_SETTINGS_INITIAL_WINDOW      (1u << 20)   /* 1 MiB */
#define HTTP2_SETTINGS_MAX_HEADER_LIST     (64u * 1024) /* 64 KiB */
#define HTTP2_SETTINGS_HEADER_TABLE_BYTES  4096u
#define HTTP2_SETTINGS_MAX_FRAME           16384u
#define HTTP2_SETTINGS_MAX_CONCURRENT      100u

/* Flood-defence constants. These are the ceilings we set on
 * nghttp2's internal queues; exceeding them makes the library answer
 * with GOAWAY(ENHANCE_YOUR_CALM) without any work on our side. */
#define HTTP2_OPT_MAX_SETTINGS      32u
#define HTTP2_OPT_MAX_OUTBOUND_ACK  64u

/* Per-stream request-body hard cap. Crossing this is
 * a stream-level error: RST_STREAM(ENHANCE_YOUR_CALM) + HttpException
 * in the handler. Sized at 10 MiB to match the HTTP/1 default
 * (src/http1/http_parser.c body_size limit) so REST POST / file upload
 * handlers behave the same on both protocols without reconfiguring. */
#define HTTP2_MAX_BODY_SIZE         (10u * 1024u * 1024u)

/* Hand-crafted GOAWAY frame for the bad-preface path. Format per
 * RFC 9113 §6.8: 9-byte frame header + 8-byte payload (last_stream_id=0,
 * error_code=PROTOCOL_ERROR=1). nghttp2 cannot emit this itself once
 * the session hit BAD_CLIENT_MAGIC, so http2_session_get_bad_preface_goaway
 * exposes these raw bytes for the caller to write directly. */
#define HTTP2_BAD_PREFACE_GOAWAY_LEN  17

const uint8_t *http2_session_bad_preface_goaway_bytes(void);
bool http2_session_should_emit_bad_preface_goaway(
                                const http2_session_t *session);

/* Lifecycle. Session owns nghttp2_session + stream table + send state.
 * Created on first feed(), freed by strategy cleanup(). Never touched
 * off-thread — single-coroutine ownership, same invariant as tls_session_t.
 *
 * @param on_request_ready  Invoked on HEADERS + END_HEADERS for each
 *                          stream (may be NULL to drop requests in
 *                          unit-test fixtures).
 * @param user_data         Opaque pointer forwarded to the callback. */
http2_session_t *http2_session_new(http_connection_t *conn,
                                   http2_request_ready_cb_t on_request_ready,
                                   void *user_data);
void             http2_session_free(http2_session_t *session);

/* Test / introspection: fetch the stream bound to @p stream_id from
 * the session's stream table. Returns NULL if no such stream exists.
 * Lets unit tests inspect stream state without reaching into the
 * private struct. */
http2_stream_t *http2_session_find_stream(http2_session_t *session,
                                          uint32_t stream_id);

/* Owning connection accessor. Per-stream handler coroutines
 * need to reach conn->handler / conn->scope / conn->server from
 * inside the session-private callback path. Returns the conn pointer
 * passed to http2_session_new (NULL in unit tests). */
http_connection_t *http2_session_get_conn(http2_session_t *session);

/* -------------------------------------------------------------------------
 * Response submission.
 * ------------------------------------------------------------------------- */

typedef struct {
    const char *name;
    size_t      name_len;
    const char *value;
    size_t      value_len;
} http2_header_view_t;

/* Submit a response on an existing stream. nghttp2 builds HEADERS
 * (with `:status` as the mandatory pseudo-header) and DATA frames
 * according to the peer's MAX_FRAME_SIZE + INITIAL_WINDOW_SIZE. The
 * body is read lazily via a data_provider that zero-copies out of the
 * buffer the caller hands us.
 *
 * Ownership: the caller must keep @p body alive until the drain loop
 * has emitted the END_STREAM DATA frame (checked via
 * http2_session_want_write returning false). In production that's the
 * PHP handler's $response object living across send_response; in
 * tests the caller holds. @p headers / name / value buffers are
 * copied by nghttp2's HPACK encoder and can be freed immediately
 * after this call returns.
 *
 * @param status     Numeric HTTP status (e.g. 200). 100-999 valid.
 * @param headers    Optional array of response headers; may be NULL
 *                   when headers_len == 0.
 * @param body       Optional response body; may be NULL when
 *                   body_len == 0 (e.g. 204/304).
 *
 * @return 0 on success, -1 on nghttp2 submit/encode error.
 */
int http2_session_submit_response(http2_session_t *session,
                                  uint32_t stream_id,
                                  int status,
                                  const http2_header_view_t *headers,
                                  size_t headers_len,
                                  const char *body,
                                  size_t body_len);

/* Submit a streaming response: HEADERS go on the wire immediately,
 * but the DATA source is the stream's chunk_queue (populated by
 * `HttpResponse::send()`). The data
 * provider returns NGHTTP2_ERR_DEFERRED whenever the queue is
 * transiently empty; caller must call
 * `nghttp2_session_resume_data(stream_id)` after each queue append
 * so nghttp2 retries the provider.
 *
 * Same headers-filter + HPACK semantics as submit_response. No body
 * arg — streaming mode owns the wire from now until
 * stream->streaming_ended + empty queue. */
int http2_session_submit_response_streaming(http2_session_t *session,
                                            uint32_t stream_id,
                                            int status,
                                            const http2_header_view_t *headers,
                                            size_t headers_len);

/* Nudge nghttp2 to re-invoke the deferred data provider (after a
 * chunk was appended or the stream was marked ended). Wraps
 * nghttp2_session_resume_data with NULL-safe guards. */
int http2_session_resume_stream_data(http2_session_t *session,
                                     uint32_t stream_id);

/* Queue a terminal HEADERS(trailers) frame for @p stream_id.
 * Must be called AFTER http2_session_submit_response but
 * BEFORE the final DATA slice drains — flipping the stream's
 * has_trailers flag makes the data_provider set
 * NGHTTP2_DATA_FLAG_NO_END_STREAM on the last slice, and nghttp2
 * emits the trailer HEADERS frame with END_STREAM set on it instead.
 *
 * Intended use: gRPC status (`grpc-status: 0`, optional `grpc-message:
 * OK`) delivered as a terminal HEADERS frame after the response body.
 *
 * @return 0 on success, -1 on nghttp2 submit/encode error or if the
 *         stream is no longer live. */
int http2_session_submit_trailer(http2_session_t *session,
                                 uint32_t stream_id,
                                 const http2_header_view_t *trailers,
                                 size_t trailers_len);

/* Feed peer bytes into the nghttp2 state machine.
 *
 * @param data         Buffer of ciphertext-decrypted / plaintext bytes.
 * @param len          Number of bytes in @p data.
 * @param consumed_out How many bytes nghttp2 actually consumed. Always
 *                     set, even on error (to 0). Caller preserves any
 *                     tail for the next feed call.
 *
 * @return 0 on success — the session may have queued outbound frames
 *          that the caller should drain via http2_session_drain.
 *         -1 on fatal framing/session error — caller tears the
 *          connection down. The connection-level error type is already
 *          encoded in the GOAWAY frame nghttp2 queued (plan §4).
 */
int http2_session_feed(http2_session_t *session,
                       const char *data, size_t len,
                       size_t *consumed_out);

/* Drain outbound bytes produced by nghttp2 into @p out_buf.
 *
 * Handles mem_send's "pointer valid until next call" contract by
 * tracking the currently-pending slice across drain calls — no copy
 * is ever partial. If @p cap is smaller than the pending slice, the
 * remaining bytes stay resident and the next drain call continues
 * from where this one left off.
 *
 * @return Number of bytes written to @p out_buf (0 = nothing to send
 *          right now), or -1 on session error.
 */
ssize_t http2_session_drain(http2_session_t *session,
                            char *out_buf, size_t cap);

/* Introspection for tests + eventual read/write pump. Both are safe
 * to call after session_new, before any feed. */
bool http2_session_want_read(const http2_session_t *session);
bool http2_session_want_write(const http2_session_t *session);

/* -------------------------------------------------------------------------
 * Graceful shutdown + PING RTT
 * ------------------------------------------------------------------------- */

/* Submit a GOAWAY frame for graceful shutdown. Uses
 * `nghttp2_submit_goaway` (NOT `terminate_session`) so in-flight
 * streams keep processing to completion — terminate_session is the
 * hard-kill variant and would strand mid-flight handlers without a
 * response. nghttp2 stamps `last-stream-id` from the highest peer-
 * initiated stream we've accepted; peers use that to decide which
 * requests are safe to retry. After this call, new peer streams are
 * refused at the nghttp2 layer; already-open streams keep running
 * until they naturally close. Caller still drains via
 * http2_session_drain.
 *
 * @param error_code RFC 9113 §7 error code (0 = NO_ERROR for normal
 *                   graceful shutdown).
 * @return 0 on success, -1 if the session is gone.
 */
int http2_session_terminate(http2_session_t *session, uint32_t error_code);

/* Submit a PING frame carrying a monotonic send-timestamp as its
 * 8-byte opaque payload. When the peer's ACK comes back,
 * on_frame_recv decodes the payload and records RTT on the session.
 * Used by gRPC-style keepalive (plan §7 — "PING RTT telemetry").
 *
 * @return 0 on success, -1 if the session is gone or submission failed. */
int http2_session_submit_ping(http2_session_t *session);

/* Last measured PING round-trip time in nanoseconds, or 0 if no ACK
 * has arrived yet. Updated on every PING-ACK we receive in response
 * to our own PING. */
uint64_t http2_session_last_ping_rtt_ns(const http2_session_t *session);

#endif /* HAVE_HTTP2 */

#endif /* HTTP2_SESSION_H */
