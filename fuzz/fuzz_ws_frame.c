/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

/*
 * libFuzzer harness for the WebSocket frame ingress (src/websocket/
 * ws_session.c). Per PLAN_WEBSOCKET.md §7: wslay itself is fuzz-tested
 * upstream, but OUR integration — the recv-buffer staging in
 * ws_session_feed(), the recv_callback copy-out, and the callback-driven
 * state in on_msg_recv_callback (FIFO append, pmce inflate, CLOSE/PONG
 * handling) — needs its own coverage.
 *
 * The harness builds a server-side session with conn == NULL (offline
 * mode — config reads fall back to defaults, no timers, no PHP objects)
 * and a discard transport, then feeds mutated bytes through
 * ws_session_feed(). That drives wslay_event_recv against a raw byte
 * stream and exercises every recv-side callback we register.
 *
 * Seed corpus: a handful of well-formed masked client frames (text,
 * binary, fragmented, ping, close) so the mutator starts from valid
 * frames and explores the protocol boundary.
 */

#include "harness_common.h"
#include "websocket/ws_session.h"

#include <stdint.h>
#include <stddef.h>

/* Discard transport — any outbound bytes wslay queues (e.g. an
 * auto-PONG or a 1009 CLOSE latched in on_msg_recv) go nowhere. We never
 * drive wslay_event_send from feed(), so these are not normally hit, but
 * binding them keeps the session self-contained. */
static bool ws_fuzz_discard(void *ctx, const uint8_t *data, size_t len)
{
    (void)ctx; (void)data; (void)len;
    return true;
}

static const ws_transport_ops_t ws_fuzz_transport = {
    .send          = ws_fuzz_discard,
    .send_internal = ws_fuzz_discard,
};

/*
 * Link-time stubs for the H1-transport / config symbols ws_session.c
 * references. With conn == NULL the recv path never reaches any of them
 * (config reads are conn-guarded; sends route through ws_fuzz_transport),
 * so these only satisfy the linker. Signatures match the production
 * prototypes in src/core/http_connection*.h. */
http_server_config_t *http_server_get_config(http_server_object *server)
{
    (void)server;
    return NULL;
}

bool http_connection_send(http_connection_t *conn, const char *data, size_t len)
{
    (void)conn; (void)data; (void)len;
    return true;
}

bool http_connection_send_batched(http_connection_t *conn, void *buf, size_t len)
{
    (void)conn; (void)buf; (void)len;
    return true;
}

bool http_connection_outbound_over_highwater(const http_connection_t *conn)
{
    (void)conn;
    return false;
}

bool http_connection_tls_fsm_send_plaintext_atomic(http_connection_t *conn,
                                                   const char *data, size_t len)
{
    (void)conn; (void)data; (void)len;
    return true;
}

/* Referenced by the static-inline stream-write-buffer getter and by
 * ws_h1_on_outbound_drain — both on the conn != NULL path only. */
const http_server_view_t *http_server_view(const http_server_object *server)
{
    (void)server;
    return NULL;
}

ws_session_t *ws_strategy_get_session(http_protocol_strategy_t *strategy)
{
    (void)strategy;
    return NULL;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    ws_session_t *session =
        ws_session_init_ex(NULL, &ws_fuzz_transport, NULL);
    if (session == NULL) {
        return 0;
    }

    /* Feed the whole mutated buffer. wslay buffers partial frames
     * internally; a protocol error returns -1 and latches teardown. */
    (void)ws_session_feed(session, data, size);

    /* Drain any messages the feed assembled so the FIFO + zend_string
     * paths are exercised and freed (mirrors WebSocket::recv()). */
    ws_pending_message_t *node;
    while ((node = ws_session_recv_pop(session)) != NULL) {
        zend_string_release(node->data);
        efree(node);
    }

    ws_session_destroy(session);
    return 0;
}
