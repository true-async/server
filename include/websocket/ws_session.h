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
#include "zend_smart_str.h"

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
 *      send_raw path. The flusher
 *      role is held cooperatively by whichever producer first finds
 *      `flushing == 0`; later producers just enqueue and return.
 *
 * The struct itself is intentionally small — heavy buffers
 * (handshake hash, frame builder scratch) live inside wslay_event
 * and are owned by it.
 */
/* Transport binding for outbound wslay bytes. H1 / wss bind to the
 * whole connection; H2 (RFC 8441) binds to a single multiplexed stream.
 * Two entries so producer-context sends (MAY suspend for backpressure)
 * and internal event-loop sends (keepalive PING / auto-PONG; MUST NOT
 * suspend) each take the right path. Both return false on a sticky
 * write error (the session is then torn down). */
typedef struct ws_transport_ops_t {
    bool (*send)(void *ctx, const uint8_t *data, size_t len);
    bool (*send_internal)(void *ctx, const uint8_t *data, size_t len);
} ws_transport_ops_t;

typedef struct ws_session_t {
    /* Owning wslay context. NULL until ws_session_init() succeeds. */
    wslay_event_context_ptr ctx;

    /* Borrowed back-pointer for callbacks. The session's lifetime is
     * a strict subset of conn's, so no refcount needed. For H2 this is
     * session->conn (shared) — config + remote-addr still resolve. */
    http_connection_t *conn;

    /* Where wslay's outbound bytes go (connection vs H2 stream). Set at
     * init. transport_ctx is the opaque target (http_connection_t* for
     * H1/wss, http2_stream_t* for H2). */
    const ws_transport_ops_t *transport;
    void                     *transport_ctx;

    /* Inbound staging buffer. Filled by ws_session_feed() from the
     * connection's read pipeline; drained by ws_session_recv_callback.
     * Sized for one max-frame worth — overflow returns WOULDBLOCK to
     * wslay so the read loop knows to suspend. */
    const uint8_t *recv_buf;
    size_t         recv_buf_len;     /* bytes available in recv_buf */
    size_t         recv_buf_pos;     /* bytes already consumed by wslay */

    /* Outbound coalescing buffer. wslay's send_callback fires once per
     * frame chunk (header, then payload); we accumulate here and flush the
     * whole frame in a single transport write after wslay_event_send —
     * halving write syscalls on the send hot path. Length reset (capacity
     * kept) after each flush; freed in ws_session_destroy. */
    smart_str      send_buf;

    /* Outbound flusher discipline.
     * Set by the producer that takes on the flusher role; cleared
     * before it returns. Other producers find it set and just enqueue.
     * Single-threaded coroutine model — no atomic primitive needed. */
    unsigned flushing : 1;

    /* Sticky write error from the send_callback path. Once set,
     * subsequent send_callback invocations short-circuit with -1
     * so wslay tears the session down on the next wslay_event_send. */
    unsigned write_error : 1;

    /* Set while an internal (non-producer) send is driving wslay — the
     * keepalive PING timer fires in event-loop context where there is no
     * coroutine to suspend, so the send_callback must use the
     * non-suspending fire-and-forget write path. See no-coroutine-in-
     * internals rule. Cleared immediately after wslay_event_send. */
    unsigned internal_send : 1;

    /* Set when on_msg_recv_callback sees opcode CLOSE (0x8) OR the
     * connection layer notifies us of peer FIN. recv() returns NULL
     * once the FIFO is drained AND this is set — distinguishing
     * "no message yet" (suspend) from "no more messages ever". */
    unsigned peer_closed : 1;

    /* Close bookkeeping for recv(): the RFC 6455 close code (0 = none /
     * plain EOF; the peer's status_code on a CLOSE frame; the code WE sent
     * on a protocol-error teardown) and the peer's optional reason text.
     * recv() returns null on a normal close (0/1000/1001/1005) and throws
     * WebSocketClosedException carrying these on any other code. */
    uint16_t     close_code;
    zend_string *close_reason;

    /* A keepalive PING was sent and we are awaiting its PONG. Set when
     * the ping timer fires, cleared when the matching PONG arrives. Gates
     * the pong-deadline timer. */
    unsigned pong_pending : 1;

    /* Outbound per-frame cap (ws_max_frame_size, cached at init; 0 =
     * disabled). Messages longer than this are auto-split into fragments
     * no larger than it. */
    uint32_t max_frame_size;

    /* Pending outbound fragmented-message sources (owns a copy of the
     * payload until wslay has read it all). Freed on eof by the read
     * callback, or wholesale in ws_session_destroy. */
    struct ws_frag_source_t *frag_sources;

#ifdef HAVE_HTTP_COMPRESSION
    /* permessage-deflate (RFC 7692). Set by ws_session_enable_pmce()
     * once negotiated; gates the RSV1 deflate/inflate paths. */
    unsigned pmce_enabled : 1;

    /* A compressed inbound message failed to inflate (zip-bomb past the
     * cap, or malformed deflate). Latched in on_msg_recv_callback;
     * ws_session_feed() returns -1 so the connection is torn down. */
    unsigned pmce_error : 1;
#endif

    /* Inbound message FIFO. Producer = ws_session_on_msg_recv_callback
     * (event-loop context), Consumer = WebSocket::recv() (coroutine
     * context). Single-threaded coroutine model — no atomics needed.
     * Head is what recv() pops next; tail is where new messages get
     * appended. */
    ws_pending_message_t *recv_head;
    ws_pending_message_t *recv_tail;

    /* Inbound FIFO memory backstop. The transport read loop never pauses,
     * so a handler that stops recv()'ing while the peer keeps sending would
     * grow the FIFO without bound (issue: ingress backpressure is out of
     * scope of the outbound plan). recv_queue_bytes tracks queued payload;
     * past recv_queue_cap (8× the per-message cap) the session closes 1013
     * and recv_overflow makes feed() return -1 (transport teardown). */
    size_t   recv_queue_bytes;
    size_t   recv_queue_cap;
    unsigned recv_overflow : 1;

    /* Trigger event that wakes a waiter blocked in recv() when a
     * message lands or the connection closes. Lazy — created on the
     * first recv() that finds the FIFO empty. */
    zend_async_trigger_event_t *recv_event;

    /* Single-reader enforcement. NULL
     * when no recv() is suspended; non-NULL while one is. A second
     * concurrent recv() throws WebSocketConcurrentReadException. */
    zend_coroutine_t *recv_waiter;

    /* Outbound backpressure (transport-level). Plain in-thread event
     * (async_plain_event — CODING_STANDARDS §1.4; a trigger event is the
     * cross-thread primitive and wrong here). Fired by the flusher / the
     * connection's batched-tail drain hook to wake a producer suspended in
     * send() over the high-water mark. Lazy — created on the first send()
     * that blocks. H1/wss only; H2 leans on its chunk-ring backpressure. */
    zend_async_event_t *drain_event;

    /* Periodic keepalive PING. Created when
     * ws_ping_interval_ms > 0 in the owning HttpServerConfig. Fires
     * every interval; the callback queues a control PING through
     * wslay and drives wslay_event_send. Stopped + disposed in
     * ws_session_destroy. NULL when keepalive is disabled. */
    zend_async_event_t          *ping_timer;
    zend_async_event_callback_t *ping_timer_cb;

    /* Pong deadline. ws_pong_timeout_ms is
     * cached from the owning HttpServerConfig at init (0 = disabled). When
     * a keepalive PING is sent, a one-shot timer is armed for that many ms;
     * the matching PONG disarms it. If it fires with pong_pending still set
     * the peer missed the liveness deadline and the connection is closed
     * 1001 Going Away. NULL when disabled or no ping is outstanding. */
    uint32_t                     pong_timeout_ms;
    zend_async_event_t          *pong_timer;
    zend_async_event_callback_t *pong_timer_cb;

#ifdef HAVE_HTTP_COMPRESSION
    /* Raw-deflate streams for permessage-deflate (windowBits -15, no
     * zlib/gzip wrapper). void* so the header stays zlib-free; the .c
     * casts to z_stream*. Reset after every message (no_context_takeover).
     * pmce_max_msg bounds the INFLATED size — wslay bounds the compressed
     * size; this is the second cap that stops a zip-bomb. */
    void   *pmce_deflate;
    void   *pmce_inflate;
    size_t  pmce_max_msg;
#endif
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

/* As ws_session_init, but with an explicit transport binding. H2 (RFC
 * 8441) binds to a stream; ws_session_init() is the H1/wss convenience
 * wrapper that binds to the whole connection. */
ws_session_t *ws_session_init_ex(http_connection_t *conn,
                                 const ws_transport_ops_t *transport,
                                 void *transport_ctx);

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
 * Drive wslay's outbound queue to the wire: serialize queued frames into
 * the per-session coalescing buffer, then flush the whole buffer in one
 * transport write. Returns wslay_event_send's rc. The caller sets
 * session->internal_send appropriately (event-loop vs producer) around
 * the call, and holds the flusher role (session->flushing).
 */
int ws_session_drive_send(ws_session_t *session);

/*
 * Queue one outbound message. When ws_max_frame_size is set and `len`
 * exceeds it, the payload is auto-split into continuation fragments no
 * larger than the cap (RFC 6455 §5.4); otherwise it goes as a single
 * frame. `rsv` carries WSLAY_RSV1_BIT for permessage-deflate, else
 * WSLAY_RSV_NONE. Returns wslay's queue rc (0 on success).
 */
int ws_session_queue_payload(ws_session_t *session, uint8_t opcode,
                             const char *data, size_t len, uint8_t rsv);

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

/* Outbound backpressure (transport-level, H1/wss). */
typedef enum {
    WS_WRITABLE_OK = 0,    /* below high-water — safe to enqueue */
    WS_WRITABLE_TIMEOUT,   /* stayed over high-water past the wait bound */
    WS_WRITABLE_CLOSED,    /* connection went away while waiting */
} ws_writable_t;

/*
 * True when the connection's outbound batched queue is at/over its
 * high-water mark. Gated on HttpServerConfig stream_write_buffer_bytes
 * (0 = disabled → always false). H1/wss only — H2 binds to a stream and
 * relies on the chunk-ring's own backpressure, so this returns false there.
 */
bool ws_session_over_highwater(const ws_session_t *session);

/*
 * Suspend the calling producer coroutine until the outbound queue drains
 * below the low-water mark (WS_WRITABLE_OK), timeout_ms elapses with the
 * queue still backed up (WS_WRITABLE_TIMEOUT), or the connection is torn
 * down while waiting (WS_WRITABLE_CLOSED). Must be called from coroutine
 * context. Only meaningful when ws_session_over_highwater() is true.
 */
ws_writable_t ws_session_wait_writable(ws_session_t *session, uint32_t timeout_ms);

/*
 * Wake any producer suspended in ws_session_wait_writable. Called by the
 * flusher after wslay_event_send drains the outbound queue, and by the
 * connection's batched-tail drain hook. No-op when nobody is waiting.
 */
void ws_session_notify_writable(ws_session_t *session);

#ifdef HAVE_HTTP_COMPRESSION
/*
 * Turn on permessage-deflate for this session: allocate the raw
 * deflate/inflate streams, allow the RSV1 frame bit, and pin the
 * decompressed-size cap from the owning server config. Must be called
 * after ws_session_init* and BEFORE the first ws_session_feed(). Returns
 * false on allocation failure (caller tears the connection down).
 */
bool ws_session_enable_pmce(ws_session_t *session);

/*
 * Compress one outbound message body per RFC 7692 §7.2.1: raw-deflate
 * with Z_SYNC_FLUSH, then drop the trailing 4-byte 00 00 FF FF marker.
 * Appends the result to `out` (caller owns it). deflateReset afterwards
 * (no_context_takeover). Returns 0 on success, -1 on a deflate error.
 */
int ws_session_pmce_deflate(ws_session_t *session,
                            const char *in, size_t in_len, smart_str *out);
#endif

#endif /* WS_SESSION_H */
