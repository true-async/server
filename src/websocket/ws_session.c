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
#include "websocket/ws_session.h"
#include "core/http_connection.h"

#include <wslay/wslay.h>

#include <errno.h>
#include <string.h>

/* {{{ ws_session_recv_callback
 *
 * wslay pulls bytes by calling this. We hand it the slice of
 * recv_buf that ws_session_feed() staged. When the buffer is
 * exhausted, signal WOULDBLOCK so wslay returns from
 * wslay_event_recv() — ws_session_feed will be called again with
 * the next chunk from the read pipeline.
 *
 * Note: we copy zero bytes because wslay's contract is "fill `buf`
 * with `len` bytes" — we instead hand back the count we have and
 * advance recv_buf_pos. wslay handles partial reads natively.
 */
static ssize_t ws_session_recv_callback(wslay_event_context_ptr ctx,
                                        uint8_t *buf, size_t len, int flags,
                                        void *user_data)
{
    (void)flags;
    ws_session_t *const s = (ws_session_t *)user_data;

    const size_t avail = s->recv_buf_len - s->recv_buf_pos;
    if (avail == 0) {
        wslay_event_set_error(ctx, WSLAY_ERR_WOULDBLOCK);
        return -1;
    }

    const size_t n = avail < len ? avail : len;
    memcpy(buf, s->recv_buf + s->recv_buf_pos, n);
    s->recv_buf_pos += n;
    return (ssize_t)n;
}
/* }}} */

/* {{{ ws_session_send_callback
 *
 * wslay pushes serialized frame bytes through this. We route them
 * through http_connection_send — the same public path the H1/H2
 * pipelines use — so TLS, kernel send-buffer backpressure, and the
 * per-connection write deadline all keep working without the WS
 * code reimplementing them.
 *
 * Per docs/PLAN_WEBSOCKET.md §2.4 the flusher discipline is held by
 * whichever PHP coroutine first picked up the producer role in
 * WebSocket::send(); other coroutines just enqueue and return. By
 * the time we get here, we ARE that coroutine, so suspension is
 * allowed and http_connection_send may suspend the coroutine on
 * write-readiness.
 *
 * On a sticky write error we set the session's write_error flag
 * (covered in feed-side decisions later) and return -1; wslay tears
 * the session down on the next wslay_event_send invocation.
 */
static ssize_t ws_session_send_callback(wslay_event_context_ptr ctx,
                                        const uint8_t *data, size_t len,
                                        int flags, void *user_data)
{
    (void)flags;
    ws_session_t *const s = (ws_session_t *)user_data;

    if (UNEXPECTED(s->write_error)) {
        wslay_event_set_error(ctx, WSLAY_ERR_CALLBACK_FAILURE);
        return -1;
    }
    if (UNEXPECTED(s->conn == NULL)) {
        wslay_event_set_error(ctx, WSLAY_ERR_CALLBACK_FAILURE);
        return -1;
    }

    if (!http_connection_send(s->conn, (const char *)data, len)) {
        s->write_error = 1;
        wslay_event_set_error(ctx, WSLAY_ERR_CALLBACK_FAILURE);
        return -1;
    }
    return (ssize_t)len;
}
/* }}} */

/* {{{ ws_ping_timer_cb_t / ws_ping_timer_fire — periodic keepalive
 *
 * Fires every ws_ping_interval_ms. Queues a zero-payload PING into
 * wslay's outbound queue and drives wslay_event_send to push it on
 * the wire. Runs in event-loop context (timer callbacks are not
 * coroutines), so the send goes through ws_session_send_callback
 * which routes through http_connection_send — which is suspending,
 * but timer callbacks DO get a coroutine assigned automatically by
 * the TrueAsync timer machinery (see how http_write_timer_cb_fn
 * uses async_io APIs from its callback).
 *
 * If the session is mid-flush by another coroutine, our queue_msg
 * still adds the frame; that flusher will drain it. We avoid stomping
 * on its in-flight wslay_event_send by checking session->flushing.
 */
typedef struct {
    zend_async_event_callback_t base;
    ws_session_t               *session;
} ws_ping_timer_cb_t;

static void ws_ping_timer_cb_dispose(
    zend_async_event_callback_t *callback, zend_async_event_t *event)
{
    (void)event;
    efree(callback);
}

static void ws_ping_timer_fire(zend_async_event_t *event,
                               zend_async_event_callback_t *callback,
                               void *result, zend_object *exception)
{
    (void)event; (void)result; (void)exception;
    ws_ping_timer_cb_t *cb = (ws_ping_timer_cb_t *)callback;
    ws_session_t *s = cb->session;
    if (s == NULL || s->ctx == NULL || s->peer_closed || s->write_error) {
        return;
    }

    /* Queue a control PING with empty payload. wslay handles framing,
     * including the FIN=1 + control-opcode bits. */
    static const struct wslay_event_msg ping = {
        .opcode     = WSLAY_PING,
        .msg        = NULL,
        .msg_length = 0,
    };
    if (wslay_event_queue_msg(s->ctx, &ping) != 0) {
        return;
    }

    /* Drive the send only if no other coroutine is currently
     * flushing — they'll pick up our frame on their next iteration.
     * This honours the same flusher-role discipline as
     * WebSocket::send() (PLAN_WEBSOCKET.md §2.4). */
    if (!s->flushing) {
        s->flushing = 1;
        (void)wslay_event_send(s->ctx);
        s->flushing = 0;
    }
}

void ws_session_arm_ping_timer(ws_session_t *s, uint32_t interval_ms)
{
    if (s->ping_timer != NULL) {
        return;
    }
    zend_async_timer_event_t *t = ZEND_ASYNC_NEW_TIMER_EVENT(
        (zend_ulong)interval_ms, /*nanoseconds=*/false);
    if (t == NULL) {
        return;
    }
    ZEND_ASYNC_TIMER_SET_MULTISHOT(t);

    ws_ping_timer_cb_t *cb = (ws_ping_timer_cb_t *)
        ZEND_ASYNC_EVENT_CALLBACK_EX(ws_ping_timer_fire, sizeof(*cb));
    if (cb == NULL) {
        t->base.dispose(&t->base);
        return;
    }
    cb->base.dispose = ws_ping_timer_cb_dispose;
    cb->session      = s;

    if (!t->base.add_callback(&t->base, &cb->base)) {
        efree(cb);
        t->base.dispose(&t->base);
        return;
    }
    if (!t->base.start(&t->base)) {
        zend_async_callbacks_remove(&t->base, &cb->base);
        t->base.dispose(&t->base);
        return;
    }
    s->ping_timer    = &t->base;
    s->ping_timer_cb = &cb->base;
}
/* }}} */

/* {{{ ws_notify_recv_waiter
 *
 * Wake whoever is suspended in WebSocket::recv() (if anyone). The
 * trigger event resolves via the standard waker pipeline; the
 * coroutine resumes inside its own ZEND_ASYNC_SUSPEND() return, then
 * inspects the FIFO / peer_closed flag to decide what to return.
 *
 * No-op when nobody is waiting — recv() will discover the new
 * message on its next entry via the FIFO check.
 */
static void ws_notify_recv_waiter(ws_session_t *s)
{
    if (s->recv_event != NULL) {
        ZEND_ASYNC_CALLBACKS_NOTIFY(&s->recv_event->base, s, NULL);
    }
}
/* }}} */

/* {{{ ws_session_on_msg_recv_callback
 *
 * Fires once per fully-reassembled WebSocket message — text or
 * binary, post-UTF-8-validation, post-defragmentation. Data
 * messages (text, binary) are appended to the FIFO so
 * WebSocket::recv() can pick them up. Control frames (PING/PONG/
 * CLOSE) are handled here without surfacing to PHP — auto-PONG
 * lands in the keepalive commit; for now CLOSE flips peer_closed
 * and PING is dropped silently (wslay's default behaviour responds
 * to PING automatically when no callback intercepts it).
 */
static void ws_session_on_msg_recv_callback(wslay_event_context_ptr ctx,
                                            const struct wslay_event_on_msg_recv_arg *arg,
                                            void *user_data)
{
    (void)ctx;
    ws_session_t *const s = (ws_session_t *)user_data;

    if (wslay_is_ctrl_frame(arg->opcode)) {
        if (arg->opcode == WSLAY_CONNECTION_CLOSE) {
            ws_session_mark_peer_closed(s);
        }
        /* PING / PONG: wslay_event auto-handles PING (queues a PONG
         * via the outbound machinery). PONG is just a keepalive
         * response — we'll consume it in the keepalive timer commit.
         * Either way, never surfaces to the PHP layer. */
        return;
    }

    /* Text (0x1) or binary (0x2) data message — enqueue. */
    ws_pending_message_t *node = emalloc(sizeof(*node));
    node->next   = NULL;
    node->binary = (arg->opcode == WSLAY_BINARY_FRAME);
    node->data   = zend_string_init((const char *)arg->msg, arg->msg_length, 0);

    if (s->recv_tail != NULL) {
        s->recv_tail->next = node;
    } else {
        s->recv_head = node;
    }
    s->recv_tail = node;

    ws_notify_recv_waiter(s);
}
/* }}} */

/* genmask is a client-side concern (RFC 6455 §5.3 — only clients mask
 * outbound frames). Server contexts never invoke it; leaving the slot
 * NULL in the callbacks struct is safe per wslay docs. */

ws_session_t *ws_session_init(http_connection_t *conn)
{
    ws_session_t *const s = ecalloc(1, sizeof(*s));
    s->conn = conn;

    static const struct wslay_event_callbacks cb = {
        .recv_callback         = ws_session_recv_callback,
        .send_callback         = ws_session_send_callback,
        .genmask_callback      = NULL,
        .on_frame_recv_start_callback   = NULL,
        .on_frame_recv_chunk_callback   = NULL,
        .on_frame_recv_end_callback     = NULL,
        .on_msg_recv_callback           = ws_session_on_msg_recv_callback,
    };

    if (wslay_event_context_server_init(&s->ctx, &cb, s) != 0) {
        efree(s);
        return NULL;
    }

    /* Arm the periodic PING keepalive if configured. Reads
     * ws_ping_interval_ms from the owning server's HttpServerConfig.
     * 0 = disabled (peer-driven keepalive only). */
    uint32_t ping_ms = 0;
    if (conn != NULL && conn->server != NULL) {
        zval *config_zv = http_server_get_config_zv(conn->server);
        if (config_zv != NULL && Z_TYPE_P(config_zv) == IS_OBJECT) {
            http_server_config_t *cfg =
                http_server_config_from_obj(Z_OBJ_P(config_zv));
            ping_ms = cfg->ws_ping_interval_ms;
        }
    }
    if (ping_ms > 0) {
        ws_session_arm_ping_timer(s, ping_ms);
    }

    /* Pull the configured cap from the owning server's frozen config
     * (the values were validated by the HttpServerConfig::setWs*
     * setters). Falls back to the documented 1 MiB default when this
     * connection has no owning server (unsupervised conn in tests).
     * Oversize messages cause wslay to fail the recv with
     * WSLAY_ERR_PROTO; the connection is then torn down — RFC 6455
     * §7.4.1 calls this "1009 Message Too Big". */
    uint64_t max_msg = 1024 * 1024;
    if (conn != NULL && conn->server != NULL) {
        zval *config_zv = http_server_get_config_zv(conn->server);
        if (config_zv != NULL && Z_TYPE_P(config_zv) == IS_OBJECT) {
            http_server_config_t *cfg =
                http_server_config_from_obj(Z_OBJ_P(config_zv));
            if (cfg->ws_max_message_size > 0) {
                max_msg = cfg->ws_max_message_size;
            }
        }
    }
    wslay_event_config_set_max_recv_msg_length(s->ctx, max_msg);

    return s;
}

void ws_session_destroy(ws_session_t *session)
{
    if (session == NULL) {
        return;
    }

    /* Tear down the keepalive timer first so a late fire cannot
     * race against the wslay context free below. The cb struct
     * is owned by the timer's callback list — disposed via remove. */
    if (session->ping_timer != NULL) {
        if (session->ping_timer_cb != NULL) {
            ((ws_ping_timer_cb_t *)session->ping_timer_cb)->session = NULL;
            zend_async_callbacks_remove(session->ping_timer,
                                        session->ping_timer_cb);
            session->ping_timer_cb = NULL;
        }
        if (session->ping_timer->loop_ref_count > 0) {
            session->ping_timer->stop(session->ping_timer);
        }
        session->ping_timer->dispose(session->ping_timer);
        session->ping_timer = NULL;
    }

    if (session->ctx) {
        wslay_event_context_free(session->ctx);
    }

    /* Drain the FIFO. Any messages that nobody got around to popping
     * leak owned zend_strings if we don't release them here. */
    ws_pending_message_t *node = session->recv_head;
    while (node != NULL) {
        ws_pending_message_t *next = node->next;
        if (node->data) {
            zend_string_release(node->data);
        }
        efree(node);
        node = next;
    }

    if (session->recv_event != NULL) {
        session->recv_event->base.dispose(&session->recv_event->base);
    }

    efree(session);
}

ws_pending_message_t *ws_session_recv_pop(ws_session_t *session)
{
    if (session == NULL || session->recv_head == NULL) {
        return NULL;
    }
    ws_pending_message_t *node = session->recv_head;
    session->recv_head = node->next;
    if (session->recv_head == NULL) {
        session->recv_tail = NULL;
    }
    node->next = NULL;
    return node;
}

void ws_session_mark_peer_closed(ws_session_t *session)
{
    if (session == NULL || session->peer_closed) {
        return;
    }
    session->peer_closed = 1;
    ws_notify_recv_waiter(session);
}

int ws_session_feed(ws_session_t *session, const uint8_t *data, size_t len)
{
    /* Stage the chunk so the recv_callback can see it, then drive
     * wslay until it stops asking for bytes (WOULDBLOCK on empty
     * buffer). On_msg_recv_callback may fire 0..N times during this
     * call depending on how many frames the chunk contains. */
    session->recv_buf     = data;
    session->recv_buf_len = len;
    session->recv_buf_pos = 0;

    const int rc = wslay_event_recv(session->ctx);

    /* Detach the borrowed pointer so a stale call site can't peek
     * into freed memory. */
    session->recv_buf     = NULL;
    session->recv_buf_len = 0;
    session->recv_buf_pos = 0;

    if (rc != 0) {
        return -1;
    }
    return 0;
}
