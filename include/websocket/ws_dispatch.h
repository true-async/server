/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

#ifndef WS_DISPATCH_H
#define WS_DISPATCH_H

#include "php.h"
#include "php_http_server.h"

typedef struct http_request_t http_request_t;

/*
 * Attempt to handle `req` as a WebSocket upgrade. Called from
 * http_connection_on_request_ready before the normal H1 dispatch
 * path runs.
 *
 * Returns:
 *   true  — request was a WS upgrade attempt; we have either
 *           accepted it (101 sent + WS handler coroutine spawned)
 *           or rejected it (4xx sent + keep_alive cleared).
 *           The caller MUST NOT proceed with H1 dispatch.
 *   false — `req` is not a WS upgrade request (no Upgrade header);
 *           the caller proceeds with normal H1 dispatch.
 *
 * No-op (returns false) when the server has no WebSocket handler
 * registered — in that case Upgrade requests fall through to the
 * H1 handler which can choose to return 426 or whatever else fits
 * its application semantics.
 */
bool ws_dispatch_try_upgrade(http_connection_t *conn, http_request_t *req);

#endif /* WS_DISPATCH_H */
