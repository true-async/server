/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

/* HTTP/1.1 static-file body delivery FSM.
 *
 * Invoked through h1_stream_ops.send_static_response by the static
 * handler once it has decided the response (status code, headers,
 * range slice, etc.) and has an open async file io handle in hand.
 *
 * Plain-TCP path:
 *   1. Serialize status line + headers off response_obj into one
 *      zend_string and submit it fire-and-forget through the existing
 *      uv_write queue (TCP_CORK gates head-vs-sendfile coalescing on
 *      Linux).
 *   2. If a body slice was requested, hand the open file_io to
 *      ZEND_ASYNC_IO_SENDFILE for kernel zero-copy transfer to the
 *      socket. Honours body_offset (Range) and body_length (slice).
 *   3. On sendfile completion, dispose file_io, uncork, fire on_done.
 *
 * TLS path:
 *   1. Encrypt+queue the head through tls_fsm_send_plaintext_atomic
 *      (single SSL_write into the BIO ring). Headers are guaranteed
 *      to fit in one record.
 *   2. Submit ZEND_ASYNC_IO_READ on file_io for a 16 KiB chunk; on
 *      completion encrypt+queue via the same atomic helper, advance
 *      the slice cursor, and either loop directly (sync-completed
 *      cipher write) or park on tls_zc_write_done_cb (async drain).
 *   3. EOF on the slice or any error path disposes file_io and fires
 *      on_done.
 *
 * No coroutine is spawned. The state machine lives entirely in
 * event-loop callback context; ownership is rooted in a single
 * persistent event callback registered on file_io->event for the
 * full duration of the chain. The HTTP/1 request lifecycle (counters,
 * http_request_finalize, keep-alive verdict) stays in the static
 * handler's on_done.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "php.h"
#include "zend_smart_str.h"
#include "Zend/zend_async_API.h"
#include "php_http_server.h"
#include "core/http_connection.h"
#include "core/http_connection_internal.h"
#ifdef HAVE_OPENSSL
#include "core/tls_layer.h"
#endif
#include "http1/http1_sendfile.h"

#include <inttypes.h>
#ifdef __linux__
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h> /* TCP_CORK */
#endif

/* TLS chunked-stream chunk size. Must be ≤ HTTP_TLS_PLAINTEXT_RING_BYTES
 * (32 KiB) so a single SSL_write never returns SSL_ERROR_WANT_WRITE in
 * the strict-ping-pong loop. 16 KiB also matches the typical TLS record
 * size, so each chunk encrypts to one record + framing overhead. */
#define H1_SEND_TLS_CHUNK_BYTES (16 * 1024)

typedef enum {
    H1_SEND_PHASE_HEAD = 0,    /* head submitted, not yet advanced */
    H1_SEND_PHASE_SENDFILE,    /* awaiting sendfile completion */
    H1_SEND_PHASE_TLS_READ,    /* awaiting ZEND_ASYNC_IO_READ chunk */
    H1_SEND_PHASE_TLS_DRAIN,   /* awaiting tls_zc_write_done_cb */
    H1_SEND_PHASE_DONE,
} h1_send_phase_t;

typedef struct {
    http_connection_t       *conn;

    /* Body source. NULL when caller passed file_io == NULL (head-only
     * inline-body responses). Disposed by this module — never by the
     * caller — once the chain finalizes. */
    zend_async_io_t         *file_io;

    /* Slice description. Plain TCP sendfile honours body_offset
     * directly; the TLS path issues sequential IO_READs and tracks
     * absolute file offset through bytes_sent + body_offset. */
    uint64_t                 body_offset;
    uint64_t                 body_length;
    uint64_t                 bytes_sent;     /* relative to body_offset */

    bool                     head_only;
    bool                     is_tls;

    h1_send_phase_t          phase;

    /* Identity guard: a callback registered during a NOTIFY iteration
     * can re-enter the same iteration with a stale result. Match req
     * identity to filter spurious fires. */
    zend_async_io_req_t     *pending_req;

    /* Persistent callback registered once on file_io->event at
     * kick-off. NULL when file_io is NULL. */
    zend_async_event_callback_t *cb;

    /* TLS chunked path scratch. emalloc'd lazily when the body branch
     * actually engages; reused across every TLS_READ → TLS_DRAIN cycle.
     * NULL on plain-TCP and head-only paths. */
    char                    *chunk_buf;

    /* Outbound completion callback. Always fires exactly once. */
    void                   (*on_done)(void *user, int status);
    void                    *user;
    int                      status; /* 0 success, -1 error */
} h1_send_state_t;

typedef struct {
    zend_async_event_callback_t base;
    h1_send_state_t            *state;
} h1_send_cb_t;

/* === Forward declarations === */
static void h1_send_dispatch(zend_async_event_t *event,
                             zend_async_event_callback_t *callback,
                             void *result, zend_object *exception);
static void h1_send_finalize(h1_send_state_t *state);
static void h1_send_handle_sendfile_done(h1_send_state_t *state);
#ifdef HAVE_OPENSSL
static void h1_send_handle_tls_read_done(h1_send_state_t *state,
                                         ssize_t bytes_read, bool err);
static void h1_send_tls_drain_done_cb(void *data);
static bool h1_send_tls_submit_next_read(h1_send_state_t *state);
#endif

static void h1_send_cb_dispose(zend_async_event_callback_t *cb,
                               zend_async_event_t *event)
{
    (void)event;
    efree(cb);
}

static inline void h1_send_state_free(h1_send_state_t *state)
{
    if (state == NULL) {
        return;
    }
    if (state->chunk_buf != NULL) {
        efree(state->chunk_buf);
        state->chunk_buf = NULL;
    }
    efree(state);
}

/* TCP_CORK gate (Linux). Headers are submitted fire-and-forget through
 * the existing batched uv_write queue; sendfile then writes directly to
 * the same fd via uv_fs_sendfile. Without serialisation, on a slow /
 * congested socket the two could interleave on the wire. Corking the
 * socket from kick-off through finalize forces the kernel to coalesce
 * (and to NEVER reorder partial writes against sendfile output) at the
 * cost of one extra setsockopt round trip per response.
 *
 * Plain-TCP only — TLS rides the BIO ring and benefits no further from
 * cork (records already self-frame). */
static inline void h1_send_cork_set(http_connection_t *conn, const int on)
{
#ifdef TCP_CORK
    if (UNEXPECTED(conn == NULL || conn->io == NULL)) {
        return;
    }
    if (conn->io->type != ZEND_ASYNC_IO_TYPE_TCP) {
        return;
    }
    const int fd = (int)conn->io->descriptor.socket;
    if (UNEXPECTED(fd < 0)) {
        return;
    }
    /* setsockopt failure here is non-fatal: worst case we skip the
     * coalescing optimisation. Sendfile + headers still go out. */
    (void)setsockopt(fd, IPPROTO_TCP, TCP_CORK, &on, sizeof(on));
#else
    (void)conn;
    (void)on;
#endif
}

/* Borrow the pre-rendered HTTP/1.1 status line from the shared table
 * in http_response.c — single source of truth for status line text.
 * Static handler only ever emits a narrow subset (200/206/304/4xx/
 * 413/500); falling back to 500 on an unknown code preserves the
 * previous behaviour. */
static const char *h1_send_status_line(const int status, size_t *out_len)
{
    const char *line = http_response_status_line_http11(status, out_len);
    if (UNEXPECTED(line == NULL)) {
        line = http_response_status_line_http11(500, out_len);
    }
    return line;
}

/* Internal accessors into http_response_object — we walk the headers
 * HashTable and body smart_str directly to keep full control over the
 * wire bytes (no auto Content-Length, no compression hooks, no
 * chunked-encoding interference). The static handler put exactly the
 * right headers on the response — we serialize them verbatim. */
extern int    http_response_get_status_code(zend_object *obj);
extern HashTable *http_response_get_headers_table(zend_object *obj);
extern zend_string *http_response_get_body_string(zend_object *obj);

/* Build status line + headers + CRLF + (optional) inline body into
 * one fresh zend_string. The caller's response_obj is read verbatim:
 * whatever Content-Length / Content-Type / etc. it carries goes onto
 * the wire as-is. No auto-Content-Length insertion — the static
 * handler is responsible for setting it (or omitting it on 304). */
static zend_string *h1_send_build_head(zend_object *response_obj,
                                       bool include_inline_body)
{
    smart_str out = {0};

    const int status_code = http_response_get_status_code(response_obj);
    size_t status_line_len = 0;
    const char *status_line = h1_send_status_line(status_code, &status_line_len);
    smart_str_appendl(&out, status_line, status_line_len);

    HashTable *const headers = http_response_get_headers_table(response_obj);
    if (headers != NULL) {
        zend_string *name;
        zval *values;
        ZEND_HASH_FOREACH_STR_KEY_VAL(headers, name, values) {
            if (UNEXPECTED(name == NULL)) {
                continue;
            }
            if (EXPECTED(Z_TYPE_P(values) == IS_STRING)) {
                smart_str_append(&out, name);
                smart_str_appendl(&out, ": ", 2);
                smart_str_append(&out, Z_STR_P(values));
                smart_str_appendl(&out, "\r\n", 2);
            } else if (Z_TYPE_P(values) == IS_ARRAY) {
                zval *val;
                ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(values), val) {
                    if (Z_TYPE_P(val) != IS_STRING) {
                        continue;
                    }
                    smart_str_append(&out, name);
                    smart_str_appendl(&out, ": ", 2);
                    smart_str_append(&out, Z_STR_P(val));
                    smart_str_appendl(&out, "\r\n", 2);
                } ZEND_HASH_FOREACH_END();
            }
        } ZEND_HASH_FOREACH_END();
    }

    /* End of headers. */
    smart_str_appendl(&out, "\r\n", 2);

    if (include_inline_body) {
        zend_string *const body = http_response_get_body_string(response_obj);
        if (body != NULL && ZSTR_LEN(body) > 0) {
            smart_str_append(&out, body);
        }
    }

    smart_str_0(&out);
    return out.s != NULL ? out.s : ZSTR_EMPTY_ALLOC();
}

/* Submit the constructed head for delivery. Plain TCP: zero-copy
 * fire-and-forget through uv_write. TLS: encrypt+kick through
 * tls_fsm_send_plaintext_atomic (atomic single-SSL_write helper —
 * with our size invariants it always fits in one record). */
static bool h1_send_submit_head(h1_send_state_t *state, zend_string *head)
{
#ifdef HAVE_OPENSSL
    if (state->is_tls) {
        const bool ok = http_connection_tls_fsm_send_plaintext_atomic(
            state->conn, ZSTR_VAL(head), ZSTR_LEN(head));
        zend_string_release(head);
        return ok;
    }
#endif
    /* Plain TCP: zero-copy fire-and-forget. send_str_owned consumes
     * the ref on success and on failure. */
    return http_connection_send_str_owned(state->conn, head);
}

/* Tear down the FSM. Disposes file_io, removes the persistent cb,
 * detaches the TLS observer, fires on_done exactly once, and frees
 * state. After this the caller's static-side counters / finalize
 * runs through on_done; nothing in this module references state
 * past this point. */
static void h1_send_finalize(h1_send_state_t *state)
{
    http_connection_t *const conn = state->conn;

    state->phase = H1_SEND_PHASE_DONE;

    /* Uncork before tearing down so the kernel flushes whatever's left
     * (typically the trailing chunk of the sendfile body). Issued
     * unconditionally — TCP_CORK off is a no-op on a non-corked socket
     * and a no-op on non-Linux. Pairs with the cork in the kick-off. */
    if (!state->is_tls) {
        h1_send_cork_set(conn, 0);
    }

#ifdef HAVE_OPENSSL
    /* Detach the TLS observer from the cipher write completion chain
     * before efree — otherwise a late free_cb fire would deref a
     * freed pointer. */
    if (state->is_tls && conn->tls_zc_write_done_cb_data == state) {
        conn->tls_zc_write_done_cb = NULL;
        conn->tls_zc_write_done_cb_data = NULL;
    }
#endif

    if (state->cb != NULL && state->file_io != NULL) {
        (void)state->file_io->event.del_callback(&state->file_io->event,
                                                 state->cb);
        state->cb = NULL;
    }

    if (state->file_io != NULL) {
        if (state->file_io->event.dispose != NULL) {
            state->file_io->event.dispose(&state->file_io->event);
        }
        state->file_io = NULL;
    }

    void (*const on_done)(void *, int) = state->on_done;
    void *const user = state->user;
    const int status = state->status;

    h1_send_state_free(state);

    if (on_done != NULL) {
        on_done(user, status);
    }
}

/* Single dispatch callback. Registered once on file_io->event for the
 * lifetime of the chain. Phase + req-identity discriminate completions;
 * spurious fires (a callback registered mid-NOTIFY can re-enter the
 * same iteration) are silently ignored. */
static void h1_send_dispatch(zend_async_event_t *event,
                             zend_async_event_callback_t *callback,
                             void *result, zend_object *exception)
{
    (void)event;
    h1_send_state_t *const state = ((h1_send_cb_t *)callback)->state;
    zend_async_io_req_t *const req = (zend_async_io_req_t *)result;

    switch (state->phase) {
    case H1_SEND_PHASE_SENDFILE:
        if (req == NULL || req != state->pending_req) {
            return;
        }
        state->pending_req = NULL;
        if (req->dispose != NULL) {
            req->dispose(req);
        }
        if (UNEXPECTED(exception != NULL)) {
            state->status = -1;
        }
        h1_send_handle_sendfile_done(state);
        return;

#ifdef HAVE_OPENSSL
    case H1_SEND_PHASE_TLS_READ: {
        if (req == NULL || req != state->pending_req) {
            return;
        }
        state->pending_req = NULL;
        const ssize_t got = req->transferred;
        const bool err = (exception != NULL || req->exception != NULL);
        if (req->exception != NULL) {
            OBJ_RELEASE(req->exception);
            req->exception = NULL;
        }
        if (req->dispose != NULL) {
            req->dispose(req);
        }
        h1_send_handle_tls_read_done(state, got, err);
        return;
    }

    case H1_SEND_PHASE_TLS_DRAIN:
        /* Drain wakeup arrives via conn->tls_zc_write_done_cb, not
         * through the file_io event. Spurious notifies on file_io
         * while parked in DRAIN are ignored. */
        return;
#endif

    case H1_SEND_PHASE_HEAD:
    case H1_SEND_PHASE_DONE:
    default:
        return;
    }
}

static void h1_send_handle_sendfile_done(h1_send_state_t *state)
{
    /* Body sent (or partially sent on error). On error we still
     * finalize — bytes already on the wire are out of our control;
     * the keep-alive verdict the static handler set decides what
     * happens to the connection next. */
    h1_send_finalize(state);
}

#ifdef HAVE_OPENSSL
/* === TLS chunked-encrypt path =====================================
 *
 * For user-space TLS connections the kernel-zero-copy sendfile path
 * is unsafe (it would put plaintext on the wire), so the body rides
 * a callback FSM that loops:
 *
 *     ZEND_ASYNC_IO_READ(file_io, chunk_buf, H1_SEND_TLS_CHUNK_BYTES)
 *       → on completion (TLS_READ → handle_tls_read_done)
 *           SSL_write(chunk_buf, n) via tls_fsm_send_plaintext_atomic
 *               → tls_fsm_send_kick submits ciphertext fire-and-forget
 *                 to libuv (zero-copy peek out of the BIO ring)
 *           if (zc_write_n == 0)            sync-complete: loop again
 *           else                            park in TLS_DRAIN
 *               → on cipher write completion (tls_zc_write_done_cb
 *                 ↦ tls_drain_done_cb)      loop again
 *     EOF (transferred == 0)               finalize
 *
 * H1_SEND_TLS_CHUNK_BYTES (16 KiB) ≤ HTTP_TLS_PLAINTEXT_RING_BYTES
 * (32 KiB) so a single SSL_write never stalls on WANT_WRITE — the
 * atomic helper succeeds in one shot every iteration.
 * ================================================================= */

static bool h1_send_tls_submit_next_read(h1_send_state_t *state)
{
    if (state->bytes_sent >= state->body_length) {
        /* Body fully shipped. Wait for any trailing in-flight cipher
         * write to finish before finalizing — but if it's already
         * settled, finalize now. The DRAIN cb path handles the not-
         * yet-settled case. */
        if (state->conn->tls_zc_write_n != 0) {
            state->phase = H1_SEND_PHASE_TLS_DRAIN;
            return true;
        }
        h1_send_finalize(state);
        return true;
    }

    state->phase = H1_SEND_PHASE_TLS_READ;
    const size_t remaining =
        (size_t)(state->body_length - state->bytes_sent);
    const size_t want = remaining < H1_SEND_TLS_CHUNK_BYTES
                            ? remaining
                            : H1_SEND_TLS_CHUNK_BYTES;
    state->pending_req = ZEND_ASYNC_IO_READ(state->file_io,
                                            state->chunk_buf, want);
    return state->pending_req != NULL;
}

static void h1_send_handle_tls_read_done(h1_send_state_t *state,
                                         ssize_t bytes_read, bool err)
{
    if (UNEXPECTED(err) || bytes_read < 0) {
        /* Read error mid-stream — bytes already on the wire belong
         * to the client, finalize and let keep-alive policy decide. */
        state->status = -1;
        h1_send_finalize(state);
        return;
    }
    if (bytes_read == 0) {
        /* EOF before requested length — file truncated under us.
         * Same recovery as the read-error path. */
        state->status = -1;
        h1_send_finalize(state);
        return;
    }

    /* Conn went into shutdown (peer FIN / worker stop) between our
     * IO_READ submit and its completion. Don't push more encrypt+
     * write traffic into a half-torn session. */
    if (state->conn->state == CONN_STATE_CLOSING ||
        state->conn->destroy_pending ||
        state->conn->tls_write_error) {
        state->status = -1;
        h1_send_finalize(state);
        return;
    }

    /* Encrypt + queue. Atomic helper bails on partial writes; with
     * H1_SEND_TLS_CHUNK_BYTES ≤ ring it always succeeds for a
     * healthy session. A failure here means the session itself is
     * wedged (sticky tls_write_error). */
    if (UNEXPECTED(!http_connection_tls_fsm_send_plaintext_atomic(
            state->conn, state->chunk_buf, (size_t)bytes_read))) {
        state->status = -1;
        h1_send_finalize(state);
        return;
    }
    state->bytes_sent += (uint64_t)bytes_read;

    /* Sync-complete cipher write (rare on Linux, common on Windows
     * try-write fast path): zc_write_n already back to 0, no DRAIN
     * wait needed. Loop directly. */
    if (state->conn->tls_zc_write_n == 0) {
        if (UNEXPECTED(!h1_send_tls_submit_next_read(state))) {
            state->status = -1;
            h1_send_finalize(state);
        }
        return;
    }

    /* Async cipher write: park until tls_zc_write_done_cb fires. */
    state->phase = H1_SEND_PHASE_TLS_DRAIN;
}

static void h1_send_tls_drain_done_cb(void *data)
{
    h1_send_state_t *state = (h1_send_state_t *)data;
    if (state == NULL) {
        return;
    }

    /* Only act when we're actually parked waiting on drain. The cb
     * also fires for non-static FSM-send completions (post-handshake
     * messages) — ignore those. */
    if (state->phase != H1_SEND_PHASE_TLS_DRAIN) {
        return;
    }

    /* Peer FIN (or worker stop) flipped the conn into CLOSING /
     * destroy_pending while we were parked on drain. Don't push
     * another encrypt+write into a torn-down session. */
    if (state->conn->state == CONN_STATE_CLOSING ||
        state->conn->destroy_pending ||
        state->conn->tls_write_error) {
        state->status = -1;
        h1_send_finalize(state);
        return;
    }

    /* Head-only / EOF reached while we were parked: caller wired us
     * into TLS_DRAIN to wait for the head's cipher write to settle.
     * Now that it has, fire on_done. */
    if (state->head_only || state->bytes_sent >= state->body_length) {
        h1_send_finalize(state);
        return;
    }

    if (UNEXPECTED(!h1_send_tls_submit_next_read(state))) {
        state->status = -1;
        h1_send_finalize(state);
    }
}
#endif /* HAVE_OPENSSL */

/* === Public entry point =========================================== */

int h1_stream_send_static_response(void *ctx_void,
                                   zend_object *response_obj,
                                   zend_async_io_t *file_io,
                                   uint64_t body_offset,
                                   uint64_t body_length,
                                   bool head_only,
                                   void (*on_done)(void *user, int status),
                                   void *user)
{
    http1_request_ctx_t *const ctx = (http1_request_ctx_t *)ctx_void;
    if (UNEXPECTED(ctx == NULL || ctx->conn == NULL)) {
        /* Pre-init failure: the caller still owns file_io. */
        return -1;
    }

    http_connection_t *const conn = ctx->conn;

    h1_send_state_t *state = ecalloc(1, sizeof(*state));
    state->conn = conn;
    state->file_io = file_io;
    state->body_offset = body_offset;
    state->body_length = body_length;
    state->head_only = head_only || file_io == NULL;
    state->on_done = on_done;
    state->user = user;
    state->status = 0;
    state->phase = H1_SEND_PHASE_HEAD;

#ifdef HAVE_OPENSSL
    state->is_tls = (conn->tls != NULL);
#else
    state->is_tls = false;
#endif

    /* Cork now so the headers write and the subsequent sendfile
     * bytes coalesce on the wire. Uncorked unconditionally in
     * finalize. Plain TCP only — TLS rides the BIO ring. */
    if (!state->is_tls && file_io != NULL && !state->head_only) {
        h1_send_cork_set(conn, 1);
    }

    /* Build head off response_obj. include_inline_body=true when
     * we have no separate body source — the response object's body
     * smart_str (small 4xx text, 416 sentinel, etc.) rides along
     * with the head. When we have a file body source the inline
     * body is skipped (caller must NOT set both). */
    const bool include_inline_body = (file_io == NULL);
    zend_string *const head = h1_send_build_head(response_obj,
                                                  include_inline_body);

    if (UNEXPECTED(!h1_send_submit_head(state, head))) {
        /* Head submit failed — nothing on the wire we need to roll
         * back beyond the cork toggle. Caller still owns file_io;
         * set state->file_io to NULL so finalize doesn't dispose
         * what we don't own. */
        if (!state->is_tls) {
            h1_send_cork_set(conn, 0);
        }
        state->file_io = NULL;
        h1_send_state_free(state);
        return -1;
    }

    /* Head-only path (HEAD requests, 304, 4xx/5xx error pages with
     * inline body). No body source to drive — fire on_done once
     * any in-flight head bytes have settled. */
    if (state->head_only) {
#ifdef HAVE_OPENSSL
        if (state->is_tls) {
            /* Head went through SSL_write; if the cipher write is
             * still in flight, park on the drain cb so on_done
             * fires after the bytes are actually queued to libuv.
             * Sync-complete: finalize directly. */
            if (conn->tls_zc_write_n != 0) {
                conn->tls_zc_write_done_cb = h1_send_tls_drain_done_cb;
                conn->tls_zc_write_done_cb_data = state;
                state->phase = H1_SEND_PHASE_TLS_DRAIN;
                return 0;
            }
        }
#endif
        /* Plain TCP fire-and-forget: from our perspective the head
         * is "sent" — libuv owns the bytes. Finalize immediately;
         * file_io is NULL on this branch so dispose is a no-op. */
        h1_send_finalize(state);
        return 0;
    }

    /* Body source present. Register the persistent dispatch callback
     * on file_io->event so completions for SENDFILE / IO_READ wake
     * us. */
    h1_send_cb_t *cb = (h1_send_cb_t *)ZEND_ASYNC_EVENT_CALLBACK_EX(
        h1_send_dispatch, sizeof(h1_send_cb_t));
    if (UNEXPECTED(cb == NULL)) {
        state->status = -1;
        h1_send_finalize(state);
        return 0;
    }
    cb->base.dispose = h1_send_cb_dispose;
    cb->state = state;
    if (UNEXPECTED(!file_io->event.add_callback(&file_io->event, &cb->base))) {
        efree(cb);
        state->status = -1;
        h1_send_finalize(state);
        return 0;
    }
    state->cb = &cb->base;

#ifdef HAVE_OPENSSL
    if (state->is_tls) {
        /* TLS body path: chunked IO_READ + SSL_write + drain loop.
         * chunk_buf is emalloc'd lazily; one buffer reused across
         * every iteration of the loop. */
        if (state->chunk_buf == NULL) {
            state->chunk_buf = emalloc(H1_SEND_TLS_CHUNK_BYTES);
        }

        /* Hook the cipher-write completion observer so the FSM-send
         * free_cb wakes us when wbio drains. Cleared in finalize. */
        conn->tls_zc_write_done_cb = h1_send_tls_drain_done_cb;
        conn->tls_zc_write_done_cb_data = state;

        /* Headers atomic-send may have left a write in flight. If so,
         * park in DRAIN and let the cb call submit_next_read once the
         * BIO ring has drained. Sync-complete: jump in directly. */
        if (conn->tls_zc_write_n != 0) {
            state->phase = H1_SEND_PHASE_TLS_DRAIN;
            return 0;
        }
        if (UNEXPECTED(!h1_send_tls_submit_next_read(state))) {
            state->status = -1;
            h1_send_finalize(state);
        }
        return 0;
    }
#endif

    /* Plain TCP body path: zero-copy kernel sendfile. body_offset
     * (Range start) and body_length (slice) are forwarded to
     * uv_fs_sendfile verbatim. */
    state->phase = H1_SEND_PHASE_SENDFILE;
    state->pending_req = ZEND_ASYNC_IO_SENDFILE(conn->io, file_io,
                                                (off_t)body_offset,
                                                (size_t)body_length);
    if (UNEXPECTED(state->pending_req == NULL)) {
        state->status = -1;
        h1_send_finalize(state);
    }
    return 0;
}
