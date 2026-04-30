/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

#ifndef WS_SESSION_H
#define WS_SESSION_H

#include "php.h"
#include "php_http_server.h"

#include <wslay/wslay.h>

/*
 * Per-connection WebSocket runtime state. Allocated when an Upgrade
 * is accepted and the WS strategy is installed; freed on connection
 * destroy.
 *
 * The struct bridges two asynchronous worlds:
 *
 *   1. Inbound: the connection's read pipeline (TLS or plaintext)
 *      pushes raw bytes into ws_session::recv_buf via
 *      ws_session_feed(). wslay_event_recv() then drains them through
 *      ws_session_recv_callback (which copies from recv_buf) and
 *      fires ws_session_on_msg_recv_callback once per assembled
 *      WebSocket message — at which point the message is handed to
 *      the handler coroutine waiting in WebSocket::recv().
 *
 *   2. Outbound: handler coroutines call wslay_event_queue_msg()
 *      from $ws->send() (any number of producers, multi-safe by
 *      construction — wslay's queue is just a FIFO of message
 *      structs). wslay_event_send() then drives
 *      ws_session_send_callback, which writes through the connection's
 *      send_raw path. Per docs/PLAN_WEBSOCKET.md §2.4 the flusher
 *      role is held cooperatively by whichever producer first finds
 *      `flushing == 0`; later producers just enqueue and return.
 *
 * The struct itself is intentionally small — heavy buffers
 * (handshake hash, frame builder scratch) live inside wslay_event
 * and are owned by it.
 */
typedef struct ws_session_t {
    /* Owning wslay context. NULL until ws_session_init() succeeds. */
    wslay_event_context_ptr ctx;

    /* Borrowed back-pointer for callbacks. The session's lifetime is
     * a strict subset of conn's, so no refcount needed. */
    http_connection_t *conn;

    /* Inbound staging buffer. Filled by ws_session_feed() from the
     * connection's read pipeline; drained by ws_session_recv_callback.
     * Sized for one max-frame worth — overflow returns WOULDBLOCK to
     * wslay so the read loop knows to suspend. */
    const uint8_t *recv_buf;
    size_t         recv_buf_len;     /* bytes available in recv_buf */
    size_t         recv_buf_pos;     /* bytes already consumed by wslay */

    /* Outbound flusher discipline. See docs/PLAN_WEBSOCKET.md §2.4.
     * Set by the producer that takes on the flusher role; cleared
     * before it returns. Other producers find it set and just enqueue.
     * Single-threaded coroutine model — no atomic primitive needed. */
    unsigned flushing : 1;

    /* Sticky write error from the send_callback path. Once set,
     * subsequent send_callback invocations short-circuit with -1
     * so wslay tears the session down on the next wslay_event_send. */
    unsigned write_error : 1;
} ws_session_t;

/*
 * Create a server-side wslay context bound to conn. Must be called
 * exactly once per connection, immediately after the HTTP/1.1 101
 * response has been written (or the H2 Extended-CONNECT stream
 * accepted, PR-2). Returns NULL on allocation failure.
 *
 * The session takes a borrowed pointer to conn — the caller is
 * responsible for ws_session_destroy() before conn is freed.
 */
ws_session_t *ws_session_init(http_connection_t *conn);

/*
 * Tear down the wslay context and free the session. Idempotent —
 * passing NULL is a no-op.
 */
void ws_session_destroy(ws_session_t *session);

/*
 * Push freshly-arrived bytes into the recv pipeline and drive
 * wslay_event_recv() to completion. Per assembled message, the
 * on_msg_recv_callback fires synchronously inside this call.
 *
 * Returns 0 on success (any partial frames are buffered inside
 * wslay), -1 on a WS protocol error or write-side failure that
 * requires connection teardown.
 */
int ws_session_feed(ws_session_t *session, const uint8_t *data, size_t len);

#endif /* WS_SESSION_H */
