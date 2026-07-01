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
#include "websocket/websocket_strategy.h"  /* ws_strategy_get_session — drain hook */
#include "core/async_plain_event.h"        /* in-thread coroutine wakeup event */
#include "core/http_connection.h"
#include "core/http_connection_internal.h"  /* http_connection_tls_fsm_send_plaintext_atomic */

#include <wslay/wslay.h>

#include <errno.h>
#include <string.h>

#ifdef HAVE_HTTP_COMPRESSION
#  ifdef HAVE_ZLIB_NG
#    include <zlib-ng.h>
#    define ZS                zng_stream
#    define ZS_DEFLATE_INIT2  zng_deflateInit2
#    define ZS_DEFLATE        zng_deflate
#    define ZS_DEFLATE_END    zng_deflateEnd
#    define ZS_DEFLATE_RESET  zng_deflateReset
#    define ZS_INFLATE_INIT2  zng_inflateInit2
#    define ZS_INFLATE        zng_inflate
#    define ZS_INFLATE_END    zng_inflateEnd
#    define ZS_INFLATE_RESET  zng_inflateReset
#  else
#    include <zlib.h>
#    define ZS                z_stream
#    define ZS_DEFLATE_INIT2  deflateInit2
#    define ZS_DEFLATE        deflate
#    define ZS_DEFLATE_END    deflateEnd
#    define ZS_DEFLATE_RESET  deflateReset
#    define ZS_INFLATE_INIT2  inflateInit2
#    define ZS_INFLATE        inflate
#    define ZS_INFLATE_END    inflateEnd
#    define ZS_INFLATE_RESET  inflateReset
#  endif
#endif

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
/* H1 / wss transport ops — wslay outbound bytes target the whole
 * connection. `send` is the producer path (suspending: backpressure +
 * TLS handled by http_connection_send). `send_internal` is the
 * event-loop path (keepalive PING / auto-PONG) which MUST NOT suspend:
 * TLS → the FSM atomic SSL_write; plaintext → the fire-and-forget
 * batched writer. transport_ctx is the http_connection_t. */
static bool ws_h1_send(void *ctx, const uint8_t *data, size_t len)
{
    return http_connection_send((http_connection_t *)ctx,
                                (const char *)data, len);
}

static bool ws_h1_send_internal(void *ctx, const uint8_t *data, size_t len)
{
    http_connection_t *const conn = (http_connection_t *)ctx;
#ifdef HAVE_OPENSSL
    if (conn->tls != NULL) {
        return http_connection_tls_fsm_send_plaintext_atomic(
            conn, (const char *)data, len);
    }
#endif
    char *copy = emalloc(len);
    memcpy(copy, data, len);
    return http_connection_send_batched(conn, copy, len);
}

static const ws_transport_ops_t ws_h1_transport = {
    .send          = ws_h1_send,
    .send_internal = ws_h1_send_internal,
};

static ssize_t ws_session_send_callback(wslay_event_context_ptr ctx,
                                        const uint8_t *data, size_t len,
                                        int flags, void *user_data)
{
    (void)flags;
    ws_session_t *const s = (ws_session_t *)user_data;

    if (UNEXPECTED(s->write_error) || UNEXPECTED(s->conn == NULL)
        || UNEXPECTED(s->transport == NULL)) {
        wslay_event_set_error(ctx, WSLAY_ERR_CALLBACK_FAILURE);
        return -1;
    }

    /* Coalesce: wslay fires this once for the frame header and again for
     * the payload. Accumulate into the per-session buffer instead of doing
     * a transport write per chunk; ws_session_drive_send flushes the whole
     * frame in one write after wslay_event_send drains the queue. */
    smart_str_appendl(&s->send_buf, (const char *)data, len);
    return (ssize_t)len;
}
/* }}} */

/* {{{ ws_session_flush_output / ws_session_drive_send
 *
 * Flush the coalescing buffer in a single transport write, then drive
 * wslay's queue through it. internal_send (keepalive / auto-reply, run in
 * event-loop context — MUST NOT suspend) vs producer ($ws->send, MAY
 * suspend for backpressure) picks the sink variant, matching the flag the
 * caller set around the drive. The buffer's length is reset (capacity
 * kept) for reuse; a failed write latches write_error so wslay tears the
 * session down on the next drive. */
static void ws_session_flush_output(ws_session_t *s)
{
    if (s->send_buf.s == NULL || ZSTR_LEN(s->send_buf.s) == 0
        || s->write_error || s->transport == NULL) {
        if (s->send_buf.s != NULL) { ZSTR_LEN(s->send_buf.s) = 0; }
        return;
    }

    const uint8_t *const data = (const uint8_t *)ZSTR_VAL(s->send_buf.s);
    const size_t len = ZSTR_LEN(s->send_buf.s);
    const bool ok = s->internal_send
        ? s->transport->send_internal(s->transport_ctx, data, len)
        : s->transport->send(s->transport_ctx, data, len);

    ZSTR_LEN(s->send_buf.s) = 0;

    if (!ok) {
        s->write_error = 1;
    }
}

int ws_session_drive_send(ws_session_t *session)
{
    const int rc = wslay_event_send(session->ctx);
    ws_session_flush_output(session);
    return rc;
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

static void ws_session_arm_pong_timer(ws_session_t *s, uint32_t timeout_ms);

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
        s->flushing      = 1;
        s->internal_send = 1;   /* timer context: non-suspending write path */
        (void)ws_session_drive_send(s);
        s->internal_send = 0;
        s->flushing      = 0;
    }

    /* Arm the pong deadline once per outstanding ping (PLAN §6.6). A peer
     * that never answers is closed 1001 after ws_pong_timeout_ms. */
    if (s->pong_timeout_ms > 0 && !s->pong_pending) {
        s->pong_pending = 1;
        ws_session_arm_pong_timer(s, s->pong_timeout_ms);
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

/* {{{ pong deadline (PLAN_WEBSOCKET.md §5/§6.6)
 *
 * One-shot timer armed when a keepalive PING is sent; disarmed by the
 * matching inbound PONG. If it fires with pong_pending still set, the
 * peer failed the RFC 6455 §5.5.2 liveness check and we close 1001.
 * Reuses ws_ping_timer_cb_t (same shape) and its dispose. */

static void ws_pong_timer_fire(zend_async_event_t *event,
                               zend_async_event_callback_t *callback,
                               void *result, zend_object *exception)
{
    (void)event; (void)result; (void)exception;
    ws_ping_timer_cb_t *cb = (ws_ping_timer_cb_t *)callback;
    ws_session_t *s = cb->session;
    if (s == NULL || s->ctx == NULL || s->peer_closed || s->write_error) {
        return;
    }
    if (!s->pong_pending) {
        return;   /* PONG landed between the deadline firing and now */
    }

    /* Peer missed the PONG deadline. Queue CLOSE 1001 and tear down. */
    s->pong_pending = 0;
    (void)wslay_event_queue_close(s->ctx, WSLAY_CODE_GOING_AWAY, NULL, 0);

    if (!s->flushing) {
        s->flushing      = 1;
        s->internal_send = 1;
        (void)ws_session_drive_send(s);
        s->internal_send = 0;
        s->flushing      = 0;
    }

    ws_session_mark_peer_closed(s);
}

static void ws_session_arm_pong_timer(ws_session_t *s, uint32_t timeout_ms)
{
    if (s->pong_timer != NULL) {
        return;
    }
    zend_async_timer_event_t *t = ZEND_ASYNC_NEW_TIMER_EVENT(
        (zend_ulong)timeout_ms, /*is_periodic=*/false);
    if (t == NULL) {
        return;
    }
    /* One-shot: is_periodic=false and no ZEND_ASYNC_TIMER_SET_MULTISHOT,
     * so it fires once — the matching PONG (disarm) or this deadline
     * (close 1001) resolves it; a healthy peer re-arms it on the next ping. */
    ws_ping_timer_cb_t *cb = (ws_ping_timer_cb_t *)
        ZEND_ASYNC_EVENT_CALLBACK_EX(ws_pong_timer_fire, sizeof(*cb));
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
    s->pong_timer    = &t->base;
    s->pong_timer_cb = &cb->base;
}

static void ws_session_disarm_pong_timer(ws_session_t *s)
{
    s->pong_pending = 0;
    if (s->pong_timer == NULL) {
        return;
    }

    if (s->pong_timer_cb != NULL) {
        ((ws_ping_timer_cb_t *)s->pong_timer_cb)->session = NULL;
        zend_async_callbacks_remove(s->pong_timer, s->pong_timer_cb);
        s->pong_timer_cb = NULL;
    }

    if (s->pong_timer->loop_ref_count > 0) {
        s->pong_timer->stop(s->pong_timer);
    }

    s->pong_timer->dispose(s->pong_timer);
    s->pong_timer = NULL;
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

#ifdef HAVE_HTTP_COMPRESSION
/* {{{ permessage-deflate (RFC 7692) — codec over the existing zlib(-ng)
 * dependency. Raw deflate/inflate (windowBits -15); per-message reset
 * (no_context_takeover); inflate is bomb-capped at pmce_max_msg. */

/* Drive deflate to completion for the bytes currently in avail_in with the
 * given flush mode, appending output to `out`. Returns 0 / -1. */
static int pmce_deflate_drain(ZS *zs, smart_str *out, const int flush)
{
    unsigned char buf[8192];
    do {
        zs->next_out  = buf;
        zs->avail_out = (unsigned)sizeof(buf);

        const int rc = ZS_DEFLATE(zs, flush);

        if (rc != Z_OK && rc != Z_BUF_ERROR) {
            return -1;
        }

        const size_t produced = sizeof(buf) - zs->avail_out;
        if (produced > 0) {
            smart_str_appendl(out, (const char *)buf, produced);
        }
    } while (zs->avail_out == 0);

    return 0;
}

int ws_session_pmce_deflate(ws_session_t *session,
                            const char *in, size_t in_len, smart_str *out)
{
    ZS *const zs = (ZS *)session->pmce_deflate;

    /* Feed input in <=1 GiB chunks so avail_in never overflows zlib's uInt;
     * only the final flush emits the sync marker. */
    const size_t          in_chunk  = (size_t)1 << 30;
    const unsigned char  *cursor    = (const unsigned char *)in;
    size_t                remaining = in_len;

    while (remaining > 0) {
        const size_t chunk = remaining < in_chunk ? remaining : in_chunk;
        zs->next_in  = (void *)(uintptr_t)cursor;
        zs->avail_in = (unsigned)chunk;

        if (pmce_deflate_drain(zs, out, Z_NO_FLUSH) != 0) {
            (void)ZS_DEFLATE_RESET(zs);
            return -1;
        }

        cursor    += chunk;
        remaining -= chunk;
    }

    zs->next_in  = NULL;
    zs->avail_in = 0;

    if (pmce_deflate_drain(zs, out, Z_SYNC_FLUSH) != 0) {
        (void)ZS_DEFLATE_RESET(zs);
        return -1;
    }

    /* Drop the trailing 00 00 FF FF empty-block marker (RFC 7692 §7.2.1).
     * Z_SYNC_FLUSH always emits at least those 4 bytes. */
    if (out->s != NULL && ZSTR_LEN(out->s) >= 4) {
        ZSTR_LEN(out->s) -= 4;
    }

    (void)ZS_DEFLATE_RESET(zs);
    return 0;
}

/* Inflate one inbound compressed message: append the synthetic
 * 00 00 FF FF tail (RFC 7692 §7.2.2), raw-inflate, and abort the moment
 * the decompressed size would exceed pmce_max_msg. On success *out_str is
 * a fresh owned zend_string. Returns 0, or -1 on bomb / malformed input. */
static int pmce_inflate_msg(ws_session_t *s,
                            const uint8_t *in, size_t in_len,
                            zend_string **out_str)
{
    ZS *const zs = (ZS *)s->pmce_inflate;
    const size_t hard_cap = s->pmce_max_msg > 0 ? s->pmce_max_msg : (1u << 20);

    static const unsigned char tail[4] = { 0x00, 0x00, 0xff, 0xff };

    size_t out_cap = 4096;
    if (out_cap > hard_cap + 1) {
        out_cap = hard_cap + 1;
    }
    zend_string *out = zend_string_alloc(out_cap, 0);
    size_t produced  = 0;
    bool   tail_fed  = false;

    zs->next_in   = (void *)(uintptr_t)in;
    zs->avail_in  = (unsigned)in_len;
    zs->next_out  = (unsigned char *)ZSTR_VAL(out);
    zs->avail_out = (unsigned)out_cap;

    for (;;) {
        const int rc = ZS_INFLATE(zs, Z_NO_FLUSH);
        produced = out_cap - zs->avail_out;

        if (produced > hard_cap) {
            ZS_INFLATE_RESET(zs);
            zend_string_release(out);
            return -1;
        }

        if (rc != Z_OK && rc != Z_BUF_ERROR && rc != Z_STREAM_END) {
            ZS_INFLATE_RESET(zs);
            zend_string_release(out);
            return -1;
        }

        if (zs->avail_in == 0 && !tail_fed) {
            tail_fed      = true;
            zs->next_in   = (void *)(uintptr_t)tail;
            zs->avail_in  = (unsigned)sizeof(tail);
            continue;
        }

        if (rc == Z_STREAM_END || zs->avail_out > 0) {
            break;
        }

        /* Output full → grow, bounded to hard_cap + 1 so an over-cap
         * payload is caught on the next pass, not after ballooning. */
        if (out_cap >= hard_cap + 1) {
            ZS_INFLATE_RESET(zs);
            zend_string_release(out);
            return -1;
        }
        size_t new_cap = out_cap * 2;
        if (new_cap > hard_cap + 1) {
            new_cap = hard_cap + 1;
        }
        out = zend_string_realloc(out, new_cap, 0);
        zs->next_out  = (unsigned char *)ZSTR_VAL(out) + produced;
        zs->avail_out = (unsigned)(new_cap - produced);
        out_cap = new_cap;
    }

    ZS_INFLATE_RESET(zs);

    if (produced != out_cap) {
        out = zend_string_truncate(out, produced, 0);
    }
    ZSTR_VAL(out)[produced] = '\0';
    *out_str = out;
    return 0;
}

bool ws_session_enable_pmce(ws_session_t *s)
{
    if (s->pmce_enabled) {
        return true;
    }
    if (s->ctx == NULL) {
        return false;
    }

    ZS *const def = ecalloc(1, sizeof(ZS));
    ZS *const inf = ecalloc(1, sizeof(ZS));

    /* windowBits -15 = raw deflate stream, no zlib/gzip wrapper. */
    if (ZS_DEFLATE_INIT2(def, Z_DEFAULT_COMPRESSION, Z_DEFLATED,
                         -15, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        efree(def);
        efree(inf);
        return false;
    }

    if (ZS_INFLATE_INIT2(inf, -15) != Z_OK) {
        ZS_DEFLATE_END(def);
        efree(def);
        efree(inf);
        return false;
    }

    s->pmce_deflate = def;
    s->pmce_inflate = inf;

    size_t cap = 1024 * 1024;
    if (s->conn != NULL && s->conn->server != NULL) {
        const http_server_config_t *cfg = http_server_get_config(s->conn->server);
        if (cfg != NULL && cfg->ws_max_message_size > 0) {
            cap = cfg->ws_max_message_size;
        }
    }
    s->pmce_max_msg = cap;

    wslay_event_config_set_allowed_rsv_bits(s->ctx, WSLAY_RSV1_BIT);
    s->pmce_enabled = 1;
    return true;
}
/* }}} */
#endif /* HAVE_HTTP_COMPRESSION */

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
        } else if (arg->opcode == WSLAY_PONG) {
            /* Liveness confirmed — disarm the pong deadline (PLAN §6.6). */
            ws_session_disarm_pong_timer(s);
        }
        /* PING: wslay_event auto-handles it (queues a PONG via the
         * outbound machinery). No control frame ever surfaces to PHP. */
        return;
    }

    /* Text (0x1) or binary (0x2) data message — enqueue. */
    zend_string *data;
#ifdef HAVE_HTTP_COMPRESSION
    if (s->pmce_enabled && wslay_get_rsv1(arg->rsv)) {
        if (pmce_inflate_msg(s, arg->msg, arg->msg_length, &data) != 0) {
            /* zip-bomb past the cap or malformed deflate. Tell the peer
             * (1009) and latch the error so feed() returns -1 and the
             * transport tears the connection/stream down — the worker
             * stays alive. mark_peer_closed wakes a recv()-suspended
             * handler at once on every transport (H1/wss/H2). */
            s->pmce_error = 1;
            (void)wslay_event_queue_close(ctx, WSLAY_CODE_MESSAGE_TOO_BIG,
                                          NULL, 0);
            ws_session_mark_peer_closed(s);
            return;
        }
    } else
#endif
    {
        data = zend_string_init((const char *)arg->msg, arg->msg_length, 0);
    }

    ws_pending_message_t *node = emalloc(sizeof(*node));
    node->next   = NULL;
    node->binary = (arg->opcode == WSLAY_BINARY_FRAME);
    node->data   = data;

    if (s->recv_tail != NULL) {
        s->recv_tail->next = node;
    } else {
        s->recv_head = node;
    }
    s->recv_tail = node;

    ws_notify_recv_waiter(s);
}
/* }}} */

/* {{{ Outbound backpressure (transport-level, H1/wss)
 *
 * The connection's batched-output queue grows under a slow consumer. The
 * transport exposes a high/low-water predicate (gated on
 * stream_write_buffer_bytes); here we bridge it to wslay producers:
 *  - on_outbound_drain fires from the batched completion path once the
 *    queue falls below the low-water mark — we wake any blocked producer;
 *  - ws_session_wait_writable parks a producer over the high-water mark.
 * H2 binds to a stream and uses the chunk-ring's own backpressure, so the
 * predicate is forced false there (transport_ctx != conn). */
void ws_session_notify_writable(ws_session_t *session)
{
    async_plain_event_fire(session->drain_event);
}

/* Connection drain hook: the H1 batched-output tail (Buffer 2) fell below
 * its low-water mark — wake any blocked producer to re-check. */
static void ws_h1_on_outbound_drain(http_connection_t *conn)
{
    ws_session_t *const s = ws_strategy_get_session(conn->strategy);
    if (s != NULL) {
        ws_session_notify_writable(s);
    }
}

bool ws_session_over_highwater(const ws_session_t *session)
{
    if (session->conn == NULL || session->conn->server == NULL) {
        return false;
    }

    const uint32_t high =
        http_server_get_stream_write_buffer_bytes(session->conn->server);
    if (high == 0) {
        return false;
    }

    /* Primary vector: the wslay outbound queue (Buffer 1), where producer
     * sends pile up while another coroutine holds the flusher role. wslay
     * tracks the exact queued byte sum for us. */
    if (session->ctx != NULL
        && wslay_event_get_queued_msg_length(session->ctx) >= high) {
        return true;
    }

    /* Secondary: the H1 batched-output tail (Buffer 2) — internal control
     * sends / future streaming. H2 binds to a stream, so skip it there. */
    return session->transport_ctx == session->conn
        && http_connection_outbound_over_highwater(session->conn);
}

ws_writable_t ws_session_wait_writable(ws_session_t *session, uint32_t timeout_ms)
{
    if (!ws_session_over_highwater(session)) {
        return WS_WRITABLE_OK;
    }

    zend_coroutine_t *const co = ZEND_ASYNC_CURRENT_COROUTINE;
    if (co == NULL || ZEND_ASYNC_IS_SCHEDULER_CONTEXT) {
        return WS_WRITABLE_OK;   /* no coroutine to suspend — caller proceeds */
    }

    if (session->drain_event == NULL) {
        session->drain_event = async_plain_event_new();
        if (session->drain_event == NULL) {
            return WS_WRITABLE_TIMEOUT;   /* degrade to backpressure signal */
        }
    }

    /* The waker owns its timeout timer (created + disposed internally — the
     * same primitive Async\delay uses, so no manual lifecycle and no timer
     * leak). On timeout it resumes cleanly (no exception); the outcome is then
     * decided by re-checking state below. Only a genuine cancellation
     * (teardown / server stop) sets an exception. */
    if (zend_async_waker_new_with_timeout(co, timeout_ms, NULL) == NULL) {
        return WS_WRITABLE_CLOSED;
    }

    zend_async_resume_when(co, session->drain_event, false,
                           zend_async_waker_callback_resolve, NULL);

    ZEND_ASYNC_SUSPEND();
    zend_async_waker_clean(co);

    if (EG(exception) != NULL) {
        return WS_WRITABLE_CLOSED;   /* cancellation — propagate */
    }

    if (session->conn == NULL || session->write_error || session->peer_closed) {
        return WS_WRITABLE_CLOSED;
    }

    /* A clean wake still over the high-water mark means the timeout fired —
     * the drain event would have dropped us below it. */
    if (ws_session_over_highwater(session)) {
        return WS_WRITABLE_TIMEOUT;
    }

    return WS_WRITABLE_OK;   /* drain fired → below low-water */
}
/* }}} */

/* genmask is a client-side concern (RFC 6455 §5.3 — only clients mask
 * outbound frames). Server contexts never invoke it; leaving the slot
 * NULL in the callbacks struct is safe per wslay docs. */

ws_session_t *ws_session_init(http_connection_t *conn)
{
    /* H1 / wss convenience wrapper: bind the transport to the whole
     * connection. H2 (RFC 8441) calls ws_session_init_ex directly with
     * a per-stream transport. */
    return ws_session_init_ex(conn, &ws_h1_transport, conn);
}

ws_session_t *ws_session_init_ex(http_connection_t *conn,
                                 const ws_transport_ops_t *transport,
                                 void *transport_ctx)
{
    ws_session_t *const s = ecalloc(1, sizeof(*s));
    s->conn          = conn;
    s->transport     = transport;
    s->transport_ctx = transport_ctx;

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
        const http_server_config_t *cfg = http_server_get_config(conn->server);
        if (cfg != NULL) {
            ping_ms = cfg->ws_ping_interval_ms;
        }
    }
    if (ping_ms > 0) {
        ws_session_arm_ping_timer(s, ping_ms);
    }

    /* Cache the pong deadline (PLAN §6.6). Only meaningful alongside the
     * ping keepalive — the deadline is armed from the ping timer fire. */
    if (conn != NULL && conn->server != NULL) {
        const http_server_config_t *cfg = http_server_get_config(conn->server);
        if (cfg != NULL) {
            s->pong_timeout_ms = cfg->ws_pong_timeout_ms;
        }
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
        const http_server_config_t *cfg = http_server_get_config(conn->server);
        if (cfg != NULL && cfg->ws_max_message_size > 0) {
            max_msg = cfg->ws_max_message_size;
        }
    }
    wslay_event_config_set_max_recv_msg_length(s->ctx, max_msg);

    /* H1/wss: wire the transport drain hook so a producer blocked over the
     * high-water mark in send() is woken when the batched queue drains.
     * transport_ctx == conn marks the H1 binding; H2 binds to a stream and
     * relies on its chunk-ring backpressure, so the hook stays unset there. */
    if (conn != NULL && transport_ctx == conn) {
        conn->on_outbound_drain = ws_h1_on_outbound_drain;
    }

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

    /* Same race guard for the one-shot pong deadline timer. */
    ws_session_disarm_pong_timer(session);

    if (session->ctx) {
        wslay_event_context_free(session->ctx);
    }

    smart_str_free(&session->send_buf);

#ifdef HAVE_HTTP_COMPRESSION
    if (session->pmce_deflate != NULL) {
        ZS_DEFLATE_END((ZS *)session->pmce_deflate);
        efree(session->pmce_deflate);
        session->pmce_deflate = NULL;
    }

    if (session->pmce_inflate != NULL) {
        ZS_INFLATE_END((ZS *)session->pmce_inflate);
        efree(session->pmce_inflate);
        session->pmce_inflate = NULL;
    }
#endif

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

    /* Unhook the transport drain callback before freeing — a late batched
     * completion must not resolve a freed session through conn->strategy. */
    if (session->conn != NULL
        && session->conn->on_outbound_drain == ws_h1_on_outbound_drain) {
        session->conn->on_outbound_drain = NULL;
    }

    if (session->drain_event != NULL) {
        session->drain_event->dispose(session->drain_event);
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

    /* wslay may have queued control replies while receiving — most
     * importantly an auto-PONG for an inbound PING (RFC 6455 §5.5.3),
     * or an auto-CLOSE on a protocol error. feed() only RECEIVES, so on
     * an otherwise-idle connection nothing else would flush them; drive
     * the send here. We run in event-loop context (the connection read
     * callback), so use the non-suspending internal send path. */
    if (session->ctx != NULL && !session->flushing
        && wslay_event_want_write(session->ctx)) {
        session->flushing      = 1;
        session->internal_send = 1;
        (void)ws_session_drive_send(session);
        session->internal_send = 0;
        session->flushing      = 0;
    }

    /* Protocol error (bad UTF-8, reserved bits, fragmented control): wslay
     * queues a CLOSE and stops reading, but recv still returns 0 with no
     * CONNECTION_CLOSE surfaced. Detect via want_read and tear down — else the
     * handler parks in recv() and the socket lingers on the peer's close echo
     * (Autobahn 6.4.x hang). */
    if (session->ctx != NULL && !session->peer_closed
        && !wslay_event_want_read(session->ctx)) {
        ws_session_mark_peer_closed(session);
        return -1;
    }

    if (rc != 0) {
        /* Hard wslay recv failure — same teardown. */
        ws_session_mark_peer_closed(session);
        return -1;
    }
#ifdef HAVE_HTTP_COMPRESSION
    /* A compressed message overflowed the cap (or was malformed): the
     * 1009 close queued in on_msg_recv was just flushed above; tear down.
     * on_msg_recv already marked peer-closed, but keep it explicit + idempotent. */
    if (session->pmce_error) {
        ws_session_mark_peer_closed(session);
        return -1;
    }
#endif
    return 0;
}
