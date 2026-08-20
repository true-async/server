/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

/*
 * HTTP/1.1 chunked-streaming vtable.
 *
 * Unlike the HTTP/2 side where stream_ops queue bytes for an nghttp2
 * data provider driven by flow-control windows, HTTP/1 chunked is a
 * straight push: format `<hex-len>\r\n<chunk>\r\n` and send. No queue,
 * no per-stream state — the kernel send buffer is the only buffering
 * we need, and http_connection_send already suspends the handler
 * coroutine when that buffer is full. Backpressure is transparent.
 *
 * Wire format per RFC 9112 §7.1:
 *     HTTP/1.1 200 OK\r\n
 *     Content-Type: text/event-stream\r\n
 *     Transfer-Encoding: chunked\r\n
 *     \r\n
 *     <hex>\r\n<chunk>\r\n
 *     <hex>\r\n<chunk>\r\n
 *     ...
 *     0\r\n\r\n
 *
 * Context pointer is the http_connection_t* itself — no per-stream
 * heap allocation, no cleanup needed. There is at most one in-flight
 * response per TCP connection (no H1 pipelining of streaming responses),
 * and the response object's streaming/headers_sent flags track state.
 */

#include "php.h"
#include "php_http_server.h"
#include "core/http_connection.h"
#include "http1/http1_stream.h"
#include "http1/http1_sendfile.h"

/* Maximum hex chunk-size line (16 hex digits for 64-bit len) + CRLF. */
#define H1_CHUNK_HEADER_MAX  18

/* Largest frame — size line, body and CRLF together — that goes out as one
 * copied write instead of three separate ones. Coalescing trades two syscalls
 * and two scheduler round-trips against one copy of the chunk, and the two are
 * worth the same somewhere between 32 and 64 KiB: measured at a 1 MiB body,
 * +25% at a 32 KiB chunk and -16% at 64 KiB (dev/BENCHMARKS.md, 2026-08-20,
 * plaintext, wrk on loopback — the crossing moves with the machine).
 *
 * The bound is on the frame and not on the chunk because of TLS, where
 * tls_push splits anything larger than the plaintext ring: a frame one byte
 * over spends a second ring cycle on a TLS record carrying six bytes. The two
 * numbers are independent — one is a measured crossing, the other a buffer
 * size — so the assert below catches them drifting apart rather than tying
 * the plaintext decision to a TLS constant. */
#define H1_CHUNK_COALESCE_MAX  (32 * 1024)

ZEND_STATIC_ASSERT(H1_CHUNK_COALESCE_MAX <= HTTP_TLS_PLAINTEXT_RING_BYTES,
                   "a coalesced frame must fit one TLS plaintext ring cycle");

/* The status line and headers of a streaming response, as bytes. Returns NULL
 * when the response is gone or formats to nothing; the caller owns the string.
 * Separate from the send so the first frame can carry the block with it. */
static zend_string *h1_streaming_headers_build(http1_request_ctx_t *ctx)
{
    http_connection_t *conn = ctx->conn;

    if (Z_ISUNDEF(ctx->response_zv)) {
        return NULL;
    }

    zend_object *response_obj = Z_OBJ(ctx->response_zv);
    /* Alt-Svc on streaming H1 responses too. Injected before
     * format_streaming_headers reads the header table; matches the
     * buffered-H1 dispose hook. No-op when the handler already set
     * the header or no H3 listener is up. */
    {
        zend_string *alt = http_server_get_alt_svc_value(conn->server);

        if (alt != NULL) {
            http_response_set_alt_svc_if_unset(
                response_obj, ZSTR_VAL(alt), ZSTR_LEN(alt));
        }
    }

    zend_string *headers =
        http_response_format_streaming_headers(response_obj);

    if (headers != NULL && ZSTR_LEN(headers) == 0) {
        zend_string_release(headers);
        return NULL;
    }

    return headers;
}

/* Headers reached the wire: the response is a streaming one from here.
 * H2 and H3 count it on their first chunk too; without the counter an H1
 * stream showed up in the send/byte totals but never in
 * streaming_responses_total. */
static void h1_stream_headers_committed(http1_request_ctx_t *ctx)
{
    ctx->h1_stream_headers_sent = true;
    http_server_on_streaming_response_started(ctx->conn->counters);
}

static bool h1_emit_headers_once(http1_request_ctx_t *ctx)
{
    zend_string *headers = h1_streaming_headers_build(ctx);

    if (headers == NULL) {
        return false;
    }

    const bool ok = http_connection_send(ctx->conn, ZSTR_VAL(headers),
                                         ZSTR_LEN(headers));
    zend_string_release(headers);
    return ok;
}

/* `nonblocking` is accepted and ignored: HTTP/1 keeps no queue of its own, so
 * there is no depth to refuse from. Backpressure here belongs to the kernel
 * socket buffer, and the only way to learn of it is to write and wait. Issue
 * #179 gives the connection one outbound queue; a refusal becomes possible
 * then, and this signature is already the one it will use. */
static int h1_stream_append_chunk(void *opaque, zend_string *chunk,
                                  const bool nonblocking)
{
    (void)nonblocking;

    http1_request_ctx_t *ctx = (http1_request_ctx_t *)opaque;

    if (ctx == NULL || ctx->conn == NULL) {
        zend_string_release(chunk);
        return HTTP_STREAM_APPEND_STREAM_DEAD;
    }

    if (ctx->stream_dead) {
        zend_string_release(chunk);
        return HTTP_STREAM_APPEND_STREAM_DEAD;
    }

    http_connection_t *conn = ctx->conn;

    if (Z_ISUNDEF(ctx->response_zv)) {
        zend_string_release(chunk);
        return HTTP_STREAM_APPEND_STREAM_DEAD;
    }

    /* First write() — commit status + headers with chunked framing. The block
     * is built here and sent with the frame below, so time-to-first-byte costs
     * one write rather than two; that matters most for SSE, where the first
     * event is the whole point and is a few dozen bytes.
     *
     * We track wire-commit on ctx->h1_stream_headers_sent rather than
     * response->committed because write() sets committed=true before
     * calling us (committed means "no more setHeader / setStatusCode
     * allowed", which happens at the PHP boundary, not on the wire). */
    zend_string *headers = NULL;

    if (!ctx->h1_stream_headers_sent) {
        headers = h1_streaming_headers_build(ctx);

        if (headers == NULL) {
            ctx->stream_dead = true;
            zend_string_release(chunk);
            return HTTP_STREAM_APPEND_STREAM_DEAD;
        }
    }

    /* Empty chunk is legal on the wire but would be indistinguishable
     * from the zero-chunk EOF marker — drop it silently. mark_ended()
     * is the only place that emits the zero-chunk. It still commits the
     * headers, which is what a handler opening a stream with one expects. */
    const size_t chunk_len = ZSTR_LEN(chunk);

    if (chunk_len == 0) {
        zend_string_release(chunk);

        if (headers != NULL) {
            const bool sent = http_connection_send(conn, ZSTR_VAL(headers),
                                                   ZSTR_LEN(headers));
            zend_string_release(headers);

            if (!sent) {
                ctx->stream_dead = true;
                return HTTP_STREAM_APPEND_STREAM_DEAD;
            }

            h1_stream_headers_committed(ctx);
        }

        http_server_on_stream_send(conn->counters, 0);
        return HTTP_STREAM_APPEND_OK;
    }

    char header[H1_CHUNK_HEADER_MAX];
    const int header_len = snprintf(header, sizeof(header),
                                    "%zx\r\n", chunk_len);

    if (header_len < 0 || (size_t)header_len >= sizeof(header)) {
        zend_string_release(chunk);

        if (headers != NULL) {
            zend_string_release(headers);
        }

        return HTTP_STREAM_APPEND_STREAM_DEAD;
    }

    /* One write per frame while the copy is cheaper than the two syscalls it
     * removes; a large chunk keeps the three-write path and stays copy-free.
     * Each http_connection_send suspends the handler until its write
     * completes, so the count of them is the count of scheduler round-trips.
     *
     * Both branches hand a buffer the caller owns to a write that outlives the
     * call when a cancellation lands mid-flight: libuv keeps the pointer until
     * its completion callback, while dispose only marks the request pending.
     * Closing that needs a write which reports its status AND takes the buffer
     * over — today's ABI offers one or the other, never both. */
    const size_t head_len  = headers != NULL ? ZSTR_LEN(headers) : 0;
    const size_t frame_len = head_len + (size_t)header_len + chunk_len + 2;
    const bool   coalesce  = frame_len <= H1_CHUNK_COALESCE_MAX;
    bool frame_ok = true;

    /* Too large to carry the block along: the headers go out on their own, and
     * the commit is recorded the moment they land rather than after the frame.
     * Anything else lets a failure in between look like headers that were
     * never sent, and mark_ended would send them a second time. */
    if (headers != NULL && !coalesce) {
        frame_ok = http_connection_send(conn, ZSTR_VAL(headers), head_len);

        if (frame_ok) {
            h1_stream_headers_committed(ctx);
        }
    }

    if (frame_ok && coalesce) {
        char *const frame = emalloc(frame_len);
        char       *at    = frame;

        if (headers != NULL) {
            memcpy(at, ZSTR_VAL(headers), head_len);
            at += head_len;
        }

        memcpy(at, header, (size_t)header_len);
        at += header_len;
        memcpy(at, ZSTR_VAL(chunk), chunk_len);
        at += chunk_len;
        memcpy(at, "\r\n", 2);

        frame_ok = http_connection_send(conn, frame, frame_len);
        efree(frame);

        if (frame_ok && headers != NULL) {
            h1_stream_headers_committed(ctx);
        }
    } else if (frame_ok) {
        frame_ok = http_connection_send(conn, header, (size_t)header_len)
                   && http_connection_send(conn, ZSTR_VAL(chunk), chunk_len)
                   && http_connection_send(conn, "\r\n", 2);
    }

    if (headers != NULL) {
        zend_string_release(headers);
    }

    if (!frame_ok) {
        /* The write is how the peer's departure becomes visible on H1 — record
         * it so isWritable() can answer without a second doomed write. */
        ctx->stream_dead = true;
        zend_string_release(chunk);
        return HTTP_STREAM_APPEND_STREAM_DEAD;
    }


    /* Each of those writes suspends. A cancellation that lands between them
     * returns success for the writes already done, leaving the frame partly
     * written — the same state a failure leaves, and it is recorded the same
     * way so mark_ended does not seal it. */
    if (UNEXPECTED(EG(exception) != NULL)) {
        ctx->stream_dead = true;
        zend_string_release(chunk);
        return HTTP_STREAM_APPEND_STREAM_DEAD;
    }

    zend_string_release(chunk);

    http_server_on_stream_send(conn->counters, chunk_len);
    /* Kernel send buffer is our backpressure mechanism, baked into
     * http_connection_send. Nothing more to report to the handler. */
    return HTTP_STREAM_APPEND_OK;
}

static void h1_stream_mark_ended(void *opaque)
{
    http1_request_ctx_t *ctx = (http1_request_ctx_t *)opaque;

    if (ctx == NULL || ctx->conn == NULL || Z_ISUNDEF(ctx->response_zv)) {
        return;
    }

    http_connection_t *conn = ctx->conn;

    /* A frame can be left half on the wire: above the coalescing threshold it
     * is three writes with a suspension between them, and a cancellation lands
     * in one of those gaps. Sealing that with a terminal chunk would tell the
     * peer the body ended cleanly and hand the connection on for reuse, and it
     * would read the terminator as the first bytes of the chunk the size line
     * promised. Refuse everything from here: no headers, no terminator, no
     * keep-alive. Checked before the header commit below, so a stream that
     * died after its headers landed does not send them twice. */
    if (UNEXPECTED(ctx->stream_dead)) {
        conn->keep_alive = false;
        return;
    }

    /* If write() was never called but mark_ended fires anyway (rare:
     * handler flipped streaming mode then immediately closed), we
     * still need to commit the headers so the peer isn't left
     * waiting for a response that never starts. */
    if (!ctx->h1_stream_headers_sent) {
        if (!h1_emit_headers_once(ctx)) {
            return;
        }

        h1_stream_headers_committed(ctx);
    }

    /* Terminal zero-chunk. Trailers not emitted — RFC requires the
     * client to opt in via TE: trailers, and the chunked-push path
     * doesn't surface a trailer API yet. */
    (void)http_connection_send(conn, "0\r\n\r\n", 5);
}

/* HTTP/1 push streaming has no internal queue — kernel backpressure
 * suspends directly inside http_connection_send — so there's nothing
 * for the handler to await on. Returning NULL signals to the write()
 * implementation that the wait-event path doesn't apply. */
static zend_async_event_t *h1_stream_get_wait_event(void *ctx)
{
    (void)ctx;
    return NULL;
}

/* Same three conditions append_chunk refuses on, asked without a chunk. */
static bool h1_stream_is_alive(void *opaque)
{
    const http1_request_ctx_t *ctx = (const http1_request_ctx_t *)opaque;

    return ctx != NULL && ctx->conn != NULL && !ctx->stream_dead
           && !ctx->conn->write_timed_out && !Z_ISUNDEF(ctx->response_zv);
}

const http_response_stream_ops_t h1_stream_ops = {
    .append_chunk         = h1_stream_append_chunk,
    .is_alive             = h1_stream_is_alive,
    .mark_ended           = h1_stream_mark_ended,
    .get_wait_event       = h1_stream_get_wait_event,
    .send_static_response = h1_stream_send_static_response,
};
