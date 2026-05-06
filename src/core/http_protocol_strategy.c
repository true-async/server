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

#include "php.h"
#include "http_protocol_strategy.h"
#include "http_connection.h"
#include "http_connection_internal.h"
#include "http1/http_parser.h"
#include <string.h>

/* HTTP/2 connection preface (RFC 7540) */
#define HTTP2_PREFACE "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n"
#define HTTP2_PREFACE_LEN 24

/* Minimum bytes needed for protocol detection */
#define MIN_DETECTION_BYTES 8

/* {{{ detect_and_assign_protocol
 *
 * Classify the first bytes of conn->read_buffer against the set of
 * protocols this server accepts and assign the matching strategy.
 * A protocol is only checked when its bit is set in the server mask;
 * disabled protocols never touch the byte stream.
 *
 * WebSocket / SSE / gRPC are not byte-prefix protocols (they negotiate
 * via HTTP/1 Upgrade or H2 content-type) so they don't appear here.
 * HTTP/3 arrives via UDP and never reaches this function.
 *
 * Idempotent — a second call after detection succeeded is a no-op.
 *
 * Returns:
 *   true  — detection complete (or already complete); conn->strategy
 *           may still be NULL if no rejection-emitting strategy was
 *           available, in which case the caller closes the connection.
 *   false — need more bytes; caller re-arms the read.
 *
 * Cross-TU: also called from http_connection_tls_feed_parser_step.
 */
bool detect_and_assign_protocol(http_connection_t *conn)
{
    if (conn->protocol_detected) {
        return true;
    }

    /* Effective mask = listener policy ∩ registered-handler set. The
     * listener mask narrows by transport (h2c-only port rejects h1 even
     * when an h1 handler is registered); the server view mask narrows
     * by handler registration (no h1 handler ⇒ no h1 accepted, even on
     * a default H1|H2 listener). Empty intersection ⇒ no protocol is
     * accepted; the caller sees a NULL strategy and closes. */
    const uint32_t server_mask = http_server_get_protocol_mask(conn->server);
    const uint32_t listener_mask = conn->protocol_mask ? conn->protocol_mask : server_mask;
    const http_protocol_mask_t mask = (http_protocol_mask_t)(listener_mask & server_mask);
    const char *const data = conn->read_buffer;
    const size_t len       = conn->read_buffer_len;

    if (UNEXPECTED(data == NULL || len < MIN_DETECTION_BYTES)) {
        return false;  /* need more bytes */
    }

    const bool h2_ok = HTTP_PROTO_MASK_HAS(mask, HTTP_PROTOCOL_HTTP2);
    const bool h1_ok = HTTP_PROTO_MASK_HAS(mask, HTTP_PROTOCOL_HTTP1);

    /*
     * Classify in-line. Outcome is one of:
     *   - HTTP_PROTOCOL_HTTP1 / HTTP_PROTOCOL_HTTP2 — recognised.
     *   - HTTP_PROTOCOL_UNSUPPORTED — bytes don't match any accepted
     *     protocol once we've seen ≥ HTTP2_PREFACE_LEN bytes (drive
     *     into h2 BAD_CLIENT_MAGIC if h2 is enabled).
     */
    http_protocol_type_t detected;

    if (h2_ok && !h1_ok) {
        /* Single-protocol fast path — let h2's framing parser emit
         * the rejection on malformed input. */
        detected = HTTP_PROTOCOL_HTTP2;
    } else if (h1_ok && !h2_ok) {
        detected = HTTP_PROTOCOL_HTTP1;
    } else if (h2_ok && memcmp(data, "PRI ", 4) == 0) {
        /* "PRI " is reserved by RFC 9112 §3 as the H2 preface prefix;
         * no valid H1 method begins with it. Match before H1 so a
         * half-mangled magic still routes to nghttp2, which emits a
         * compliant GOAWAY(PROTOCOL_ERROR). */
        detected = HTTP_PROTOCOL_HTTP2;
    } else if (h1_ok && (data[0] == 'G' || data[0] == 'P' || data[0] == 'D'
                      || data[0] == 'H' || data[0] == 'O' || data[0] == 'C'
                      || data[0] == 'T')) {
        /* HTTP/1 method dispatch by first byte (G/P/D/H/O/C/T). llhttp
         * re-validates the full method inside the H1 strategy. */
        detected = HTTP_PROTOCOL_HTTP1;
    } else if (len >= HTTP2_PREFACE_LEN) {
        /* Past one preface worth of bytes and still unrecognised. If
         * H1 is enabled, hand off to llhttp so it emits an RFC 400;
         * otherwise UNSUPPORTED routes to h2 BAD_CLIENT_MAGIC. */
        detected = h1_ok ? HTTP_PROTOCOL_HTTP1 : HTTP_PROTOCOL_UNSUPPORTED;
    } else {
        return false;  /* still ambiguous — need more bytes */
    }

    switch (detected) {
        case HTTP_PROTOCOL_HTTP1:
            conn->protocol_type = HTTP_PROTOCOL_HTTP1;
            conn->strategy      = http_protocol_strategy_http1_create();
            break;

        case HTTP_PROTOCOL_HTTP2:
            conn->protocol_type = HTTP_PROTOCOL_HTTP2;
            conn->strategy      = http_protocol_strategy_http2_create();
            /* h2 disabled at build-time but mask said it was OK?
             * Degrade to h1 if the server also accepts it. */
            if (UNEXPECTED(conn->strategy == NULL) && h1_ok) {
                conn->protocol_type = HTTP_PROTOCOL_HTTP1;
                conn->strategy      = http_protocol_strategy_http1_create();
            }
            break;

        default:
            /* HTTP_PROTOCOL_UNSUPPORTED — drive into nghttp2 if h2 is
             * enabled (BAD_CLIENT_MAGIC → GOAWAY); otherwise the
             * caller closes silently on the empty strategy. */
            if (h2_ok) {
                conn->protocol_type = HTTP_PROTOCOL_HTTP2;
                conn->strategy      = http_protocol_strategy_http2_create();
            }
            break;
    }

    if (conn->strategy) {
        conn->strategy->on_request_ready = http_connection_on_request_ready;
    }
    conn->protocol_detected = true;
    return true;
}
/* }}} */

/* {{{ http_protocol_strategy_destroy */
void http_protocol_strategy_destroy(http_protocol_strategy_t *strategy)
{
    if (strategy) {
        efree(strategy);
    }
}
/* }}} */

/*
 * HTTP/1.1 Strategy Implementation
 *
 * Wraps the pooled http1_parser_t on conn->parser, drives llhttp via
 * http_parser_execute(), and fires on_request_ready from inside
 * llhttp's on_headers_complete (via the callback attached to the
 * parser).
 */

/* {{{ http1_feed
 *
 * Run llhttp over a chunk of bytes from the connection buffer.
 *
 * Dispatch fires from INSIDE llhttp's on_headers_complete, via the
 * callback we attached to the parser. http1_feed itself does not
 * check is_complete after execute — by that point dispatch has already
 * happened (or never will, for this chunk); on_message_complete
 * triggers body_event.
 */
static int http1_feed(http_protocol_strategy_t *strategy,
                      http_connection_t *conn,
                      const char *data,
                      size_t len,
                      size_t *consumed_out)
{
    /* Lazy parser acquisition + attach dispatch wiring once. The
     * parser keeps conn + dispatch_cb until
     * http_parser_reset_for_reuse / destroy clears them. */
    if (!conn->parser) {
        conn->parser = parser_pool_acquire();
        if (!conn->parser) {
            if (consumed_out) { *consumed_out = 0; }
            return -1;
        }
        http_parser_attach(conn->parser, conn, strategy->on_request_ready);
    }

    int result = http_parser_execute(conn->parser, data, len, consumed_out);
    if (result != 0) {
        return -1;
    }

    return 0;
}
/* }}} */

static void http1_strategy_send_response(http_connection_t *conn, void *response)
{
    /* TODO: move response formatting in here. */
    (void)conn;
    (void)response;
}

static void http1_strategy_reset(http_connection_t *conn)
{
    /* Keep-alive reset is handled by the connection coroutine
     * (parser returned to pool, acquired fresh for next request). */
    (void)conn;
}

static void http1_strategy_cleanup(http_connection_t *conn)
{
    /* Parser lifetime is still managed by the connection; no strategy-
     * owned resources to free beyond the strategy struct itself. */
    (void)conn;
}

/* {{{ http_protocol_strategy_http1_create */
http_protocol_strategy_t* http_protocol_strategy_http1_create(void)
{
    http_protocol_strategy_t *strategy = ecalloc(1, sizeof(http_protocol_strategy_t));

    strategy->protocol_type = HTTP_PROTOCOL_HTTP1;
    strategy->name = "HTTP/1.1";
    strategy->feed = http1_feed;
    strategy->on_request_ready = NULL;  /* set by connection layer */
    strategy->send_response = http1_strategy_send_response;
    strategy->reset = http1_strategy_reset;
    strategy->cleanup = http1_strategy_cleanup;

    return strategy;
}
/* }}} */

/*
 * HTTP/2 Strategy Implementation.
 *
 * The real implementation lives in src/http2/http2_strategy.c and is only
 * compiled when --enable-http2 succeeded (HAVE_HTTP2). When HTTP/2 is
 * disabled at build time, the stub below returns NULL and the connection
 * layer falls back to HTTP/1 — keeping detect_and_assign_protocol
 * single-sourced regardless of build configuration.
 */
#ifndef HAVE_HTTP2
/* {{{ http_protocol_strategy_http2_create (stub for --disable-http2 builds) */
http_protocol_strategy_t* http_protocol_strategy_http2_create(void)
{
    return NULL;
}
/* }}} */
#endif
