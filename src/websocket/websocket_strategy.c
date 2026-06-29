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
#include "websocket/websocket_strategy.h"
#include "websocket/ws_session.h"
#include "core/http_connection.h"
#include "core/http_protocol_strategy.h"

#include <wslay/wslay.h>
#include <wslay/wslayver.h>

/*
 * Per-connection WS strategy state. Same wrapper pattern as
 * http2_strategy_t in src/http2/http2_strategy.c — the connection
 * layer only ever sees the embedded `base` and routes feed/cleanup
 * through its function pointers. Casting back to the wrapper is safe
 * because http_protocol_strategy_websocket_create is the sole
 * producer of these objects.
 *
 * The session is created lazily on the first feed() call rather than
 * at strategy create time so the strategy struct can be installed
 * before the handshake-emit step has finalised any session-level
 * configuration (subprotocol, extensions). Until session creation,
 * incoming bytes are buffered in conn->read_buffer by the read loop,
 * exactly as for any other protocol.
 */
typedef struct {
    http_protocol_strategy_t base;
    ws_session_t            *session;   /* lazy on first feed() */
    http_connection_t       *conn;      /* captured for callbacks */
} ws_strategy_t;

static int ws_feed(http_protocol_strategy_t *strategy,
                   http_connection_t *conn,
                   const char *data,
                   size_t len,
                   size_t *consumed_out)
{
    ws_strategy_t *const self = (ws_strategy_t *)strategy;

    /* Lazy session init. Keeps the strategy struct cheap on
     * connections that have been swapped to WS but haven't yet
     * received their first frame (the handshake-response write may
     * complete before the client's first frame arrives). */
    if (UNEXPECTED(self->session == NULL)) {
        self->conn    = conn;
        self->session = ws_session_init(conn);
        if (self->session == NULL) {
            /* OOM during wslay_event_context_server_init. Connection
             * layer will close — there is no graceful close-frame
             * path because we never had a session to send through. */
            if (consumed_out) { *consumed_out = 0; }
            return -1;
        }
    }

    /* Hand the chunk to the session — wslay_event_recv runs to
     * completion inside this call, firing on_msg_recv_callback per
     * complete WebSocket message. The whole chunk is consumed (no
     * partial-frame buffering on our side; wslay buffers internally). */
    const int rc = ws_session_feed(self->session,
                                   (const uint8_t *)data, len);
    if (consumed_out) {
        *consumed_out = (rc == 0) ? len : 0;
    }
    return rc;
}

static void ws_send_response(http_connection_t *conn, void *response)
{
    /* WebSocket has no HTTP response after the upgrade — every byte
     * on the wire is a frame, written through ws_stream_ops. */
    (void)conn;
    (void)response;
}

static void ws_reset(http_connection_t *conn)
{
    /* WS connections are single-shot — no keep-alive reset semantics. */
    (void)conn;
}

static void ws_cleanup(http_connection_t *conn)
{
    /* The connection layer calls cleanup() on conn->strategy. We
     * cannot reach the wrapper through `conn` directly (we only have
     * the strategy via conn->strategy), so route through the same
     * pointer. ws_session_destroy is NULL-safe. */
    ws_strategy_t *const self = (ws_strategy_t *)conn->strategy;
    if (self != NULL) {
        ws_session_destroy(self->session);
        self->session = NULL;
        self->conn    = NULL;
    }
}

http_protocol_strategy_t* http_protocol_strategy_websocket_create(void)
{
    ws_strategy_t *self = ecalloc(1, sizeof(*self));

    self->base.protocol_type    = HTTP_PROTOCOL_WEBSOCKET;
    self->base.name             = "WebSocket/" WSLAY_VERSION;
    self->base.feed             = ws_feed;
    self->base.on_request_ready = NULL;  /* WS dispatches the whole session, not per-request */
    self->base.send_response    = ws_send_response;
    self->base.reset            = ws_reset;
    self->base.cleanup          = ws_cleanup;

    return &self->base;
}

ws_session_t *ws_strategy_ensure_session(http_protocol_strategy_t *strategy,
                                         http_connection_t *conn)
{
    if (strategy == NULL || strategy->protocol_type != HTTP_PROTOCOL_WEBSOCKET) {
        return NULL;
    }
    ws_strategy_t *self = (ws_strategy_t *)strategy;
    if (self->session == NULL) {
        self->conn    = conn;
        self->session = ws_session_init(conn);
    }
    return self->session;
}

ws_session_t *ws_strategy_get_session(http_protocol_strategy_t *strategy)
{
    if (strategy == NULL || strategy->protocol_type != HTTP_PROTOCOL_WEBSOCKET) {
        return NULL;
    }
    return ((ws_strategy_t *)strategy)->session;
}
