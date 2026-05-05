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
#include "Zend/zend_async_API.h"

#include <wslay/wslay.h>

/*
 * One reassembled WebSocket message waiting in the session FIFO.
 * Linked-list node — small enough that the per-message allocation
 * cost is negligible. `data` is owned by this node; `binary` mirrors
 * the wslay opcode (0x1 = text → false, 0x2 = binary → true).
 */
typedef struct ws_pending_message_t {
    struct ws_pending_message_t *next;
    zend_string                 *data;
    bool                         binary;
} ws_pending_message_t;

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
 *      WebSocket message — at which point the message is appended to
 *      the recv FIFO and any waiter in WebSocket::recv() is woken.
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

    /* Set when on_msg_recv_callback sees opcode CLOSE (0x8) OR the
     * connection layer notifies us of peer FIN. recv() returns NULL
     * once the FIFO is drained AND this is set — distinguishing
     * "no message yet" (suspend) from "no more messages ever". */
    unsigned peer_closed : 1;

    /* Inbound message FIFO. Producer = ws_session_on_msg_recv_callback
     * (event-loop context), Consumer = WebSocket::recv() (coroutine
     * context). Single-threaded coroutine model — no atomics needed.
     * Head is what recv() pops next; tail is where new messages get
     * appended. */
    ws_pending_message_t *recv_head;
    ws_pending_message_t *recv_tail;

    /* Trigger event that wakes a waiter blocked in recv() when a
     * message lands or the connection closes. Lazy — created on the
     * first recv() that finds the FIFO empty. */
    zend_async_trigger_event_t *recv_event;

    /* Single-reader enforcement (docs/PLAN_WEBSOCKET.md §6.9). NULL
     * when no recv() is suspended; non-NULL while one is. A second
     * concurrent recv() throws WebSocketConcurrentReadException. */
    zend_coroutine_t *recv_waiter;

    /* Periodic keepalive PING (PLAN_WEBSOCKET.md §6.6). Created when
     * ws_ping_interval_ms > 0 in the owning HttpServerConfig. Fires
     * every interval; the callback queues a control PING through
     * wslay and drives wslay_event_send. Stopped + disposed in
     * ws_session_destroy. NULL when keepalive is disabled. */
    zend_async_event_t          *ping_timer;
    zend_async_event_callback_t *ping_timer_cb;
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

/*
 * Pop the head message from the recv FIFO. Returns the node (caller
 * owns it; must zend_string_release(node->data) and efree(node)) or
 * NULL when the FIFO is empty.
 */
ws_pending_message_t *ws_session_recv_pop(ws_session_t *session);

/*
 * Mark the session as peer-closed (no further messages will arrive).
 * Idempotent. Wakes any waiter currently suspended in recv() so it
 * sees the closed state and returns NULL.
 */
void ws_session_mark_peer_closed(ws_session_t *session);

#endif /* WS_SESSION_H */
