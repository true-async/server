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

#endif /* WEBSOCKET_STRATEGY_H */
