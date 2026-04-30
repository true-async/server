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
#include "core/http_connection.h"
#include "core/http_protocol_strategy.h"

#include <wslay/wslay.h>
#include <wslay/wslayver.h>

/*
 * SCAFFOLD ONLY.
 *
 * This file currently registers a strategy that compiles, links, and
 * is structurally complete, but its feed/cleanup paths are deliberate
 * stubs. Real frame I/O, the wslay_event_context wiring, the
 * recv/send/genmask callbacks, and the upgrade-handshake path land in
 * follow-up commits per docs/PLAN_WEBSOCKET.md §8.
 *
 * Keeping the scaffold separate from the behaviour PRs lets later
 * changes be reviewed as pure additions to a known-good skeleton
 * rather than as one large landing.
 */

static int ws_feed(http_protocol_strategy_t *strategy,
                   http_connection_t *conn,
                   const char *data,
                   size_t len,
                   size_t *consumed_out)
{
    (void)strategy;
    (void)conn;
    (void)data;
    (void)len;

    /* Frame parsing not implemented yet — drop bytes silently and
     * report end-of-stream so the connection layer treats this as a
     * clean shutdown rather than spinning. The handshake path will
     * not yet swap into this strategy until the implementation
     * arrives, so this branch is unreachable in production. */
    if (consumed_out) {
        *consumed_out = len;
    }
    return 0;
}

static void ws_send_response(http_connection_t *conn, void *response)
{
    /* WebSocket has no HTTP response after the upgrade — every byte on
     * the wire is a frame, written through ws_stream_ops. */
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
    /* wslay_event_context teardown lands with the real implementation. */
    (void)conn;
}

http_protocol_strategy_t* http_protocol_strategy_websocket_create(void)
{
    http_protocol_strategy_t *strategy = ecalloc(1, sizeof(http_protocol_strategy_t));

    strategy->protocol_type    = HTTP_PROTOCOL_WEBSOCKET;
    strategy->name             = "WebSocket/" WSLAY_VERSION;
    strategy->feed             = ws_feed;
    strategy->on_request_ready = NULL;  /* WS has no per-request dispatch — handler runs for the whole session */
    strategy->send_response    = ws_send_response;
    strategy->reset            = ws_reset;
    strategy->cleanup          = ws_cleanup;

    return strategy;
}
