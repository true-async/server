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

/* Maximum hex chunk-size line (16 hex digits for 64-bit len) + CRLF. */
#define H1_CHUNK_HEADER_MAX  18

static bool h1_emit_headers_once(http1_request_ctx_t *const ctx)
{
    http_connection_t *const conn = ctx->conn;
    if (Z_ISUNDEF(ctx->response_zv)) {
        return false;
    }
    zend_object *const response_obj = Z_OBJ(ctx->response_zv);
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
    zend_string *const headers =
        http_response_format_streaming_headers(response_obj);
    if (headers == NULL || ZSTR_LEN(headers) == 0) {
        if (headers != NULL) {
            zend_string_release(headers);
        }
        return false;
    }
    const bool ok = http_connection_send(conn, ZSTR_VAL(headers),
                                         ZSTR_LEN(headers));
    zend_string_release(headers);
    return ok;
}

static int h1_stream_append_chunk(void *const opaque, zend_string *const chunk)
{
    http1_request_ctx_t *const ctx = (http1_request_ctx_t *)opaque;
    if (ctx == NULL || ctx->conn == NULL) {
        zend_string_release(chunk);
        return HTTP_STREAM_APPEND_STREAM_DEAD;
    }
    http_connection_t *const conn = ctx->conn;
    if (Z_ISUNDEF(ctx->response_zv)) {
        zend_string_release(chunk);
        return HTTP_STREAM_APPEND_STREAM_DEAD;
    }

    /* First send() — commit status + headers with chunked framing.
     * We track wire-commit on ctx->h1_stream_headers_sent rather than
     * response->committed because send() sets committed=true before
     * calling us (committed means "no more setHeader / setStatusCode
     * allowed", which happens at the PHP boundary, not on the wire). */
    if (!ctx->h1_stream_headers_sent) {
        if (!h1_emit_headers_once(ctx)) {
            zend_string_release(chunk);
            return HTTP_STREAM_APPEND_STREAM_DEAD;
        }
        ctx->h1_stream_headers_sent = true;
    }

    /* Empty chunk is legal on the wire but would be indistinguishable
     * from the zero-chunk EOF marker — drop it silently. mark_ended()
     * is the only place that emits the zero-chunk. */
    const size_t chunk_len = ZSTR_LEN(chunk);
    if (chunk_len == 0) {
        zend_string_release(chunk);
        http_server_on_stream_send(conn->counters, 0);
        return HTTP_STREAM_APPEND_OK;
    }

    char header[H1_CHUNK_HEADER_MAX];
    const int header_len = snprintf(header, sizeof(header),
                                    "%zx\r\n", chunk_len);
    if (header_len < 0 || (size_t)header_len >= sizeof(header)) {
        zend_string_release(chunk);
        return HTTP_STREAM_APPEND_STREAM_DEAD;
    }

    if (!http_connection_send(conn, header, (size_t)header_len) ||
        !http_connection_send(conn, ZSTR_VAL(chunk), chunk_len) ||
        !http_connection_send(conn, "\r\n", 2)) {
        zend_string_release(chunk);
        return HTTP_STREAM_APPEND_STREAM_DEAD;
    }
    zend_string_release(chunk);

    http_server_on_stream_send(conn->counters, chunk_len);
    /* Kernel send buffer is our backpressure mechanism, baked into
     * http_connection_send. Nothing more to report to the handler. */
    return HTTP_STREAM_APPEND_OK;
}

static void h1_stream_mark_ended(void *const opaque)
{
    http1_request_ctx_t *const ctx = (http1_request_ctx_t *)opaque;
    if (ctx == NULL || ctx->conn == NULL || Z_ISUNDEF(ctx->response_zv)) {
        return;
    }
    http_connection_t *const conn = ctx->conn;

    /* If send() was never called but mark_ended fires anyway (rare:
     * handler flipped streaming mode then immediately closed), we
     * still need to commit the headers so the peer isn't left
     * waiting for a response that never starts. */
    if (!ctx->h1_stream_headers_sent) {
        if (!h1_emit_headers_once(ctx)) {
            return;
        }
        ctx->h1_stream_headers_sent = true;
    }

    /* Terminal zero-chunk. Trailers not emitted — RFC requires the
     * client to opt in via TE: trailers, and the chunked-push path
     * doesn't surface a trailer API yet. */
    (void)http_connection_send(conn, "0\r\n\r\n", 5);
}

/* HTTP/1 push streaming has no internal queue — kernel backpressure
 * suspends directly inside http_connection_send — so there's nothing
 * for the handler to await on. Returning NULL signals to the send()
 * implementation that the wait-event path doesn't apply. */
static zend_async_event_t *h1_stream_get_wait_event(void *const ctx)
{
    (void)ctx;
    return NULL;
}

const http_response_stream_ops_t h1_stream_ops = {
    .append_chunk   = h1_stream_append_chunk,
    .mark_ended     = h1_stream_mark_ended,
    .get_wait_event = h1_stream_get_wait_event,
};
