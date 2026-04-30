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
 * wslay pushes serialized frame bytes through this. The actual
 * socket write — including TLS, suspending send_raw, and the
 * flusher-role discipline from PLAN_WEBSOCKET.md §2.4 — lands in a
 * follow-up commit alongside the public send() PHP method.
 *
 * Until then the callback short-circuits with WOULDBLOCK so any
 * accidental wslay_event_send() invocation returns cleanly without
 * tearing the session down. Producers do not yet exist (no PHP API)
 * so this branch is unreachable in practice.
 */
static ssize_t ws_session_send_callback(wslay_event_context_ptr ctx,
                                        const uint8_t *data, size_t len,
                                        int flags, void *user_data)
{
    (void)data;
    (void)len;
    (void)flags;
    (void)user_data;

    wslay_event_set_error(ctx, WSLAY_ERR_WOULDBLOCK);
    return -1;
}
/* }}} */

/* {{{ ws_session_on_msg_recv_callback
 *
 * Fires once per fully-reassembled WebSocket message — text or
 * binary, post-UTF-8-validation, post-defragmentation. Hand-off to
 * the handler coroutine waiting in WebSocket::recv() lands with the
 * PHP API commit; for now the message is dropped silently.
 *
 * Control frames (PING/PONG/CLOSE) are surfaced here too via
 * arg->opcode. PING auto-response and CLOSE-handshake handling will
 * be filtered out in the same future commit so the handler only ever
 * sees data messages.
 */
static void ws_session_on_msg_recv_callback(wslay_event_context_ptr ctx,
                                            const struct wslay_event_on_msg_recv_arg *arg,
                                            void *user_data)
{
    (void)ctx;
    (void)arg;
    (void)user_data;
    /* TODO: deliver to handler coroutine. */
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

    return s;
}

void ws_session_destroy(ws_session_t *session)
{
    if (session == NULL) {
        return;
    }
    if (session->ctx) {
        wslay_event_context_free(session->ctx);
    }
    efree(session);
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
