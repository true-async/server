/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

#ifndef WEBSOCKET_STRATEGY_H
#define WEBSOCKET_STRATEGY_H

#include "php_http_server.h"
#include "core/http_protocol_strategy.h"

typedef struct ws_session_t ws_session_t;

/*
 * WebSocket protocol strategy (RFC 6455 over HTTP/1.1, RFC 8441 over
 * HTTP/2). Installed on a connection AFTER a successful Upgrade
 * handshake — never the result of byte-prefix detection. The H1 path
 * dispatches the upgrade from the H1 strategy and swaps conn->strategy
 * in-place; the H2 path (PR-2) installs ws_stream_ops on a single
 * Extended-CONNECT stream without replacing the H2 session.
 *
 * Frame parsing, masking, fragmentation reassembly, UTF-8 validation
 * and control-frame invariants are delegated to bundled wslay
 * (deps/wslay/), called via the standard wslay_event_callbacks bound
 * to the connection. See docs/PLAN_WEBSOCKET.md §2.1.
 */
http_protocol_strategy_t* http_protocol_strategy_websocket_create(void);

/*
 * Eagerly create the wslay session inside an already-installed
 * WS strategy. Normally feed() does this lazily on the first byte
 * arrival, but ws_dispatch needs the session up-front so it can
 * hand its pointer to the freshly-created WebSocket PHP object
 * before the user handler runs (recv() is otherwise unable to
 * locate the session from a NULL slot).
 *
 * Idempotent — a second call is a no-op. Returns the session, or
 * NULL on allocation failure (caller must tear the connection down).
 */
ws_session_t *ws_strategy_ensure_session(http_protocol_strategy_t *strategy,
                                         http_connection_t *conn);

/*
 * Read-only accessor. Returns the live session (NULL until
 * ensure_session or feed() has been called).
 */
ws_session_t *ws_strategy_get_session(http_protocol_strategy_t *strategy);

#endif /* WEBSOCKET_STRATEGY_H */
