/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+

  TLS connection — coroutine producer + scheduler drain.

  tls_push is the only producer entry and must run from a coroutine.
  It waits on tls_space_event until the plaintext ring has room for
  len bytes, BIO_writes them in one atomic call, and kicks the drain.

  tls_drain is scheduler-side and never yields. It pulls plaintext via
  SSL_write into the cipher BIO and submits one ciphertext span via
  WRITE_EX. tls_cipher_completion (the WRITE_EX free_cb) consumes the
  shipped cipher slot, signals ring space, and re-enters the drain so
  the chain self-sustains until the rings are empty.

  The read FSM is unchanged: a persistent callback on io->event reads
  ciphertext into the BIO and drives tls_advance_state. FSM-produced
  handshake / alert bytes ship through the same drain.
*/

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include "http_connection_internal.h"   /* php.h + Zend/zend_async_API.h +
                                         * http_connection.h + http1/http_parser.h
                                         * + <limits.h> + <stdint.h> */
#include "Zend/zend_hrtime.h"
#include "Zend/zend_exceptions.h"        /* zend_clear_exception */
#include "php_http_server.h"             /* http_server_on_tls_*, http_server_object */
#include "http_protocol_strategy.h"      /* ALPN fast-path strategy creation */
#include "tls_layer.h"                   /* tls_session_t + tls_io_result_t */
#include "log/http_log.h"

#include <openssl/bio.h>
#include <openssl/ssl.h>

#include <string.h>

bool tls_drain(http_connection_t *conn);   /* exported for h2 emit pump */
static void tls_signal_space(http_connection_t *conn);

static bool tls_decrypt_into_buffer(http_connection_t *conn);
static int  tls_feed_parser_step(http_connection_t *conn);
static void tls_graceful_close(http_connection_t *conn);
static void tls_flush_pending_alert(http_connection_t *conn);
static void tls_log_error(const http_connection_t *conn, const char *context);
static void tls_absorb_io_submission_exception(const http_connection_t *conn,
                                               const char *op);

static bool tls_fsm_io_cb_attach(http_connection_t *conn);
static void tls_cipher_completion(void *data, zend_async_io_t *io);
static void tls_fsm_io_callback_fn(zend_async_event_t *event,
                                   zend_async_event_callback_t *callback,
                                   void *result, zend_object *exception);
static void tls_fsm_io_callback_dispose(zend_async_event_callback_t *callback,
                                        zend_async_event_t *event);

static bool tls_arm_one_shot_read(http_connection_t *conn);

static void tls_advance_state(http_connection_t *conn);
static bool tls_finalize_if_closing(http_connection_t *conn);

/* ========================================================================
 * Producer + drain (issue #23).
 *
 * tls_push:  coroutine-only. Waits for `len` bytes of ring space, writes
 *            atomically, kicks the drain.
 * tls_drain: scheduler-side, never yields. SSL_writes plaintext into the
 *            cipher BIO, submits one cipher span via WRITE_EX.
 * tls_cipher_completion: WRITE_EX free_cb. Consumes the shipped slot,
 *            signals ring space, re-enters the drain.
 * ======================================================================== */

static void tls_signal_space(http_connection_t *conn)
{
    if (conn->tls_space_event != NULL) {
        conn->tls_space_event->trigger(conn->tls_space_event);
    }
}

void tls_release_waiters(http_connection_t *conn)
{
    if (conn->tls_space_event != NULL) {
        conn->tls_space_event->base.dispose(&conn->tls_space_event->base);
        conn->tls_space_event = NULL;
    }
}

/* Park the current coroutine until BIO_ctrl_get_write_guarantee returns
 * at least @p need bytes. The wake-and-recheck loop absorbs spurious
 * wakes and producers racing on shared space. Caller MUST be in a
 * coroutine — scheduler-context drain bypasses this helper and
 * BIO_write's directly under its own room check. */
static bool tls_wait_space(http_connection_t *conn, size_t need)
{
    if (ZEND_ASYNC_IS_SCHEDULER_CONTEXT) {
        return false;
    }

    while ((size_t)BIO_ctrl_get_write_guarantee(conn->tls_plaintext_bio) < need) {
        if (conn->tls_space_event == NULL) {
            conn->tls_space_event = ZEND_ASYNC_NEW_TRIGGER_EVENT();

            if (conn->tls_space_event == NULL) {
                return false;
            }
        }

        zend_coroutine_t *co = ZEND_ASYNC_CURRENT_COROUTINE;

        if (UNEXPECTED(ZEND_ASYNC_WAKER_NEW(co) == NULL)) {
            return false;
        }

        zend_async_resume_when(co, &conn->tls_space_event->base, false,
                               zend_async_waker_callback_resolve, NULL);

        ZEND_ASYNC_SUSPEND();
        zend_async_waker_clean(co);

        if (EG(exception) != NULL || conn->tls == NULL || conn->tls_write_error) {
            return false;
        }
    }

    return true;
}

/* Move plaintext from the BIO ring through SSL_write into the cipher
 * BIO and submit one ciphertext span via WRITE_EX. Never yields. The
 * tls_draining re-entrancy guard folds the sync-complete fast path:
 * tls_cipher_completion calls back into tls_drain inline and sees the
 * guard, so the outer pass loops once more on its return — turning a
 * freed cipher BIO into more plaintext progress without an extra
 * event-loop tick. */
bool tls_drain(http_connection_t *conn)
{
    if (conn->tls == NULL || conn->tls_write_error) {
        return false;
    }

    if (conn->tls_draining) {
        return true;
    }

    conn->tls_draining = 1;
    bool ok = true;

    for (;;) {
        bool progress = false;

        /* Single-in-flight gate: while libuv holds a slot pointer into
         * network_bio, do not run a fresh SSL_write. SSL_write would
         * BIO_write more ciphertext into the same ring, and the BIO
         * pair reclaims space lazily — peek/consume cycles can hand the
         * same offset back to a fresh write, racing libuv's still-
         * pending read of the in-flight span (peer "bad record mac"
         * under load). Serialise encrypt-and-ship on cipher_inflight;
         * plaintext_bio absorbs producer backpressure (32 KiB ring) and
         * h2 frames queue inside nghttp2's own outbound queue until the
         * completion callback frees the slot. */
        if (conn->tls_cipher_inflight == 0) {
            /* 1) Push plaintext through SSL_write. */
            char     *plain = NULL;
            const int avail = BIO_nread0(conn->tls_plaintext_bio_app, &plain);

            if (avail > 0) {
                size_t                written = 0;
                const tls_io_result_t rc = tls_write_plaintext(
                    conn->tls, plain, (size_t)avail, &written);

                if (written > 0) {
                    char     *dummy    = NULL;
                    const int consumed = BIO_nread(conn->tls_plaintext_bio_app,
                                                   &dummy, (int)written);

                    if (UNEXPECTED(consumed != (int)written)) {
                        ok = false;
                        break;
                    }

                    http_server_on_tls_io(conn->counters, 0, written, 0, 0);
                    tls_signal_space(conn);
                    progress = true;
                }

                if (rc != TLS_IO_OK && rc != TLS_IO_WANT_WRITE && rc != TLS_IO_WANT_READ) {
                    ok = false;
                    break;
                }
            }
        }

        /* 2) Submit one cipher span when no write is in flight. The
         *    completion callback (tls_cipher_completion) consumes the
         *    BIO ring, signals plaintext-ring space, and re-enters
         *    this drain so the chain self-sustains. */
        if (conn->tls_cipher_inflight == 0) {
            if (UNEXPECTED(!tls_fsm_io_cb_attach(conn))) {
                ok = false;
                break;
            }

            char        *slot         = NULL;
            const size_t cipher_avail = tls_peek_cipher_out(conn->tls, &slot);

            if (cipher_avail > 0) {
                /* Zero-copy: slot points into network_bio. Safe because
                 * (a) step 1 above is gated on cipher_inflight, so no
                 * concurrent SSL_write will mutate the BIO ring, and
                 * (b) tls_cipher_completion calls BIO_nread to consume
                 * only after libuv has released the buffer. The slot
                 * bytes are stable for the entire WRITE_EX lifetime. */
                conn->tls_cipher_inflight = cipher_avail;
                const zend_async_io_req_t *req = ZEND_ASYNC_IO_WRITE_EX(
                    conn->io, slot, cipher_avail, tls_cipher_completion);

                if (UNEXPECTED(req == NULL)) {
                    conn->tls_cipher_inflight = 0;
                    tls_absorb_io_submission_exception(conn, "write");
                    ok = false;
                    break;
                }

                if (req->completed) {
                    /* Sync-complete: free_cb already ran consume + cleared
                     * cipher_inflight. Loop back to push more plaintext. */
                    progress = true;
                    continue;
                }
            }
        }

        if (!progress) {
            break;
        }
    }

    conn->tls_draining = 0;

    if (!ok) {
        conn->tls_write_error = true;
    }

    return ok && !conn->tls_write_error;
}

/* Producer entry. Coroutine-only.
 *
 * For len ≤ ring_size: one atomic BIO_write after the wait. For
 * len > ring_size (h1 large body): the call chunks internally;
 * single-coroutine h1 has no concurrent producer so the chunks stay
 * contiguous on the wire. */
bool tls_push(http_connection_t *conn, const char *data, const size_t len)
{
    if (conn->tls_write_error) {
        return false;
    }

    if (len == 0) {
        return true;
    }

    size_t off = 0;
    while (off < len) {
        const size_t guarantee = HTTP_TLS_PLAINTEXT_RING_BYTES;
        const size_t need      = (len - off) < guarantee ? (len - off) : guarantee;

        if (!tls_wait_space(conn, need)) {
            return false;
        }

        const int n = BIO_write(conn->tls_plaintext_bio, data + off, (int)need);

        if (UNEXPECTED(n != (int)need)) {
            conn->tls_write_error = true;
            return false;
        }

        off += need;

        if (!tls_drain(conn)) {
            return false;
        }
    }

    return true;
}

/* ========================================================================
 * Read FSM async-send path (event-loop callback context)
 *
 * The read FSM produces ciphertext during handshake, post-handshake
 * messages, alerts, and close_notify. It cannot suspend, so the bytes
 * are copied into a private heap buffer and submitted as a non-blocking
 * ZEND_ASYNC_IO_WRITE. The persistent send-completion callback frees
 * the buffer and re-enters tls_advance_state.
 *
 * Coordination with the scheduler-side drain (tls_drain): the FSM only
 * runs during handshake / alert / close_notify, so after handshake the
 * drain side owns all plaintext-→-ciphertext shipping.
 * ======================================================================== */

/* Read-side dispatch callback. The FSM-send path is fire-and-forget
 * via ZEND_ASYNC_IO_WRITE_EX (no NOTIFY) so only reads route through
 * here. Renamed-but-typedef'd as tls_fsm_io_cb_t below for the rest of
 * the file. */
struct _tls_fsm_send_cb {
    zend_async_event_callback_t  base;
    http_connection_t           *conn;
    zend_async_io_req_t         *read_req;     /* outstanding read, NULL when idle */
};

/* Compatibility alias for the rest of the file — the unified struct
 * fills both the send-callback and read-callback roles. */
typedef tls_fsm_send_cb_t tls_fsm_io_cb_t;

static void tls_fsm_io_callback_dispose(
    zend_async_event_callback_t *callback,
    zend_async_event_t *event)
{
    (void)event;
    /* By contract, destroy never disposes this callback while a
     * write is in flight: the connection-level destroy gate
     * (http_connection_destroy → destroy_pending) waits on the FSM
     * write completion. The read req is libuv-owned and gets cleaned
     * up by io_close_cb when the io handle is torn down. */
    efree(callback);
}

static void tls_fsm_io_callback_fn(
    zend_async_event_t *event,
    zend_async_event_callback_t *callback,
    void *result,
    zend_object *exception)
{
    (void)event;
    tls_fsm_io_cb_t *cb = (tls_fsm_io_cb_t *)callback;
    http_connection_t *conn = cb->conn;

    if (UNEXPECTED(conn == NULL)) {
        return;
    }

    /* Dispatch by read req identity. Writes are fire-and-forget via
     * WRITE_EX and never route through this NOTIFY path. */
    if (cb->read_req == NULL || result != cb->read_req) {
        return;
    }

    /* Read completion. */
    {
        zend_async_io_req_t *req = cb->read_req;
        const ssize_t bytes_read = req->transferred;
        const bool err = (exception != NULL) || (req->exception != NULL);

        if (UNEXPECTED(req->exception != NULL)) {
            OBJ_RELEASE(req->exception);
            req->exception = NULL;
        }

        cb->read_req = NULL;
        req->dispose(req);

        if (UNEXPECTED(err || bytes_read <= 0)) {
            conn->state = CONN_STATE_CLOSING;
            tls_advance_state(conn);
            (void)tls_finalize_if_closing(conn);
            return;
        }

        if (UNEXPECTED(!tls_commit_cipher_in(conn->tls, (size_t)bytes_read))) {
            conn->state = CONN_STATE_CLOSING;
            tls_advance_state(conn);
            (void)tls_finalize_if_closing(conn);
            return;
        }

        http_server_on_tls_io(conn->counters, 0, 0, (size_t)bytes_read, 0);

        tls_advance_state(conn);

        if (tls_finalize_if_closing(conn)) {
            return;
        }

        if (!conn->tls_awaiting_handler) {
            if (UNEXPECTED(!tls_arm_one_shot_read(conn))) {
                conn->state = CONN_STATE_CLOSING;
                tls_advance_state(conn);
                (void)tls_finalize_if_closing(conn);
            }
        }
    }
}

/* Lazily attach the unified io callback. Stored on conn->tls_fsm_send_cb
 * so the destroy path can find it through the existing field; the read
 * path locates it via the same pointer through tls_fsm_read_attach. */
static bool tls_fsm_io_cb_attach(http_connection_t *conn)
{
    if (conn->tls_fsm_send_cb != NULL) {
        return true;
    }

    if (conn->io == NULL) {
        return false;
    }

    tls_fsm_io_cb_t *cb = (tls_fsm_io_cb_t *)
        ZEND_ASYNC_EVENT_CALLBACK_EX(tls_fsm_io_callback_fn,
                                     sizeof(tls_fsm_io_cb_t));

    if (UNEXPECTED(cb == NULL)) {
        return false;
    }

    cb->base.dispose    = tls_fsm_io_callback_dispose;
    cb->conn            = conn;
    cb->read_req        = NULL;

    if (UNEXPECTED(!conn->io->event.add_callback(&conn->io->event, &cb->base))) {
        tls_fsm_io_callback_dispose(&cb->base, NULL);
        return false;
    }

    conn->tls_fsm_send_cb = cb;
    return true;
}

/* Zero-copy fire-and-forget completion: consume the BIO output ring slot
 * libuv just finished writing, count the bytes, then re-advance the FSM
 * (which will produce more ciphertext via SSL_write and re-kick). The
 * `data` arg is the BIO ring pointer we passed in — opaque to us here;
 * the size is stamped on conn->tls_cipher_inflight at submit time so the
 * consume call below knows how much to release.
 *
 * Errors are silent here (libuv's free_cb contract doesn't surface
 * status); a failed write makes the next read attempt fail and the conn
 * tears down through the existing peer-FIN path. */
static void tls_cipher_completion(void *data, zend_async_io_t *io)
{
    /* data is the network_bio slot pointer we handed to libuv —
     * libuv-side accounting only; the actual ring consume happens via
     * BIO_nread below, now that libuv is done reading from the slot. */
    (void)data;

    if (UNEXPECTED(io == NULL || io->user_data == NULL)) {
        return;
    }

    http_connection_t *conn = (http_connection_t *)io->user_data;

    if (UNEXPECTED(conn->tls == NULL)) {
        return;
    }

    const size_t n = conn->tls_cipher_inflight;
    conn->tls_cipher_inflight = 0;

    if (UNEXPECTED(n == 0)) {
        return;
    }

    if (UNEXPECTED(!tls_consume_cipher_out(conn->tls, n))) {
        conn->tls_write_error = true;
    } else {
        http_server_on_tls_io(conn->counters, 0, 0, 0, n);
    }

    /* Connection-level destroy was deferred while this write was in
     * flight (http_connection_destroy sees fsm_send_in_flight and sets
     * destroy_pending). Now that the buffer is released, run the
     * pending teardown. */
    if (UNEXPECTED(conn->destroy_pending) && conn->handler_refcount == 0) {
        conn->destroy_pending = false;
        http_connection_destroy(conn);
        return;
    }

    /* Drain any ciphertext the FSM produced while we were waiting, and
     * keep the state machine moving (e.g. a queued close_notify after
     * the just-completed alert). */
    tls_advance_state(conn);

    if (tls_finalize_if_closing(conn)) {
        return;
    }

    /* Plaintext BIO just gained room — drive the h2 emit pump
     * synchronously to drain any frames nghttp2 has queued. Same
     * scheduler context as http2_session_emit runs in; tls_drain's
     * tls_draining guard handles re-entrancy. No-op on plain HTTP/1
     * connections (NULL strategy / non-h2). */
    {
        extern void http2_conn_notify_emit(http_connection_t *);
        http2_conn_notify_emit(conn);
    }

    /* Static FSM observer hook: the static TLS path needs to know when
     * wbio has drained so it can encrypt the next file chunk without
     * SSL_ERROR_WANT_WRITE. Fire after tls_advance_state so any post-
     * handshake bytes have already been re-kicked through this chain
     * (and the observer sees a settled BIO state). */
    if (conn->tls_zc_write_done_cb != NULL) {
        void (*cb)(void *) = conn->tls_zc_write_done_cb;
        void *cb_data      = conn->tls_zc_write_done_cb_data;
        cb(cb_data);
    }
}

/* Atomic-encrypt-and-queue helper for callers that want to send a small
 * plaintext message from FSM context — currently only the parse-error
 * path. The response fits in one SSL_write (< 1 KiB; TLS record / wbio
 * is 17 KiB), so a single call always succeeds when the session is
 * healthy. Returns true on full encrypt + kick. */
bool http_connection_tls_fsm_send_plaintext_atomic(http_connection_t *conn,
                                                   const char *data,
                                                   size_t len)
{
    if (conn->tls == NULL || conn->tls_write_error || len == 0) {
        return false;
    }

    size_t written = 0;
    const tls_io_result_t rc = tls_write_plaintext(conn->tls, data, len, &written);

    if (rc != TLS_IO_OK || written != len) {
        return false;
    }

    http_server_on_tls_io(conn->counters, 0, written, 0, 0);
    tls_drain(conn);
    return true;
}

/* ========================================================================
 * Read FSM (event-loop callback context)
 *
 * The read callback drives a state machine on conn->state:
 *
 *   CONN_STATE_TLS_HANDSHAKE
 *       SSL_do_handshake → kick async send of any wbio bytes;
 *       OK transitions to CONN_STATE_READING_HEADERS.
 *
 *   CONN_STATE_READING_HEADERS / READING_BODY / KEEPALIVE_WAIT
 *       SSL_read → fill conn->read_buffer → feed strategy parser;
 *       on dispatch, mark tls_awaiting_handler and idle the FSM until
 *       http_connection_tls_resume_after_handler re-enters.
 *
 *   CONN_STATE_PROCESSING / SENDING
 *       Handler in flight; FSM idle (tls_awaiting_handler == true).
 *
 *   CONN_STATE_CLOSING
 *       Best-effort close_notify, then destroy when no FSM send is
 *       in flight.
 *
 * Read scheduling:
 *   - One-shot read into a BIO slot via tls_reserve_cipher_in. Keeps
 *     the cipher pipeline zero-copy for ciphertext.
 *   - Re-armed by the read callback after each chunk, unless the FSM
 *     is awaiting a handler or has transitioned to CLOSING.
 * ======================================================================== */

/* Submit one ciphertext read directly into a BIO slot. Returns true if
 * the read is armed (or completed sync); false on submission failure
 * (caller transitions to CLOSING). The unified io callback's read_req
 * slot tracks the outstanding req. */
static bool tls_arm_one_shot_read(http_connection_t *conn)
{
    if (UNEXPECTED(!tls_fsm_io_cb_attach(conn))) {
        return false;
    }

    tls_fsm_io_cb_t *cb = conn->tls_fsm_send_cb;

    if (UNEXPECTED(cb->read_req != NULL)) {
        return true;   /* already armed */
    }

    char *slot = NULL;
    const size_t space = tls_reserve_cipher_in(conn->tls, &slot);

    if (UNEXPECTED(space == 0 || slot == NULL)) {
        /* Cipher BIO full — the decrypt step should have drained it
         * before we got here. Treat as a hard error. */
        return false;
    }

    zend_async_io_req_t *req = ZEND_ASYNC_IO_READ(conn->io, slot, space);

    if (UNEXPECTED(req == NULL)) {
        tls_absorb_io_submission_exception(conn, "read");
        return false;
    }

    /* Sync-complete fast path: the libuv layer occasionally surfaces a
     * synchronous EAGAIN-then-data sequence on Windows + some Linux
     * kernels. Handle inline so we don't bounce through the event loop
     * when the bytes are already there. */
    if (req->completed) {
        const bool err = (req->exception != NULL);
        const ssize_t bytes_read = req->transferred;

        if (req->exception != NULL) {
            OBJ_RELEASE(req->exception);
            req->exception = NULL;
        }

        req->dispose(req);

        if (UNEXPECTED(err || bytes_read <= 0)) {
            return false;
        }

        if (UNEXPECTED(!tls_commit_cipher_in(conn->tls, (size_t)bytes_read))) {
            return false;
        }

        http_server_on_tls_io(conn->counters, 0, 0, (size_t)bytes_read, 0);
        tls_advance_state(conn);

        if (tls_finalize_if_closing(conn)) {
            return true;
        }

        if (!conn->tls_awaiting_handler) {
            return tls_arm_one_shot_read(conn);
        }

        return true;
    }

    cb->read_req = req;
    return true;
}

/* ------------------------------------------------------------------------
 * Helpers used by the FSM body (decrypt, feed, close, alert, log).
 * ------------------------------------------------------------------------ */

/* Pull every byte of plaintext OpenSSL currently has for us out of the
 * session and append it to conn->read_buffer, growing the buffer if
 * necessary. Returns true on successful drain (including the benign
 * WANT_READ that just means "BIO is empty, come back later"); false on
 * a hard TLS error. */
/* Last-line cap; HTTP parser limits (414/431/413) trip first on
 * healthy traffic. Bounds RAM if a peer streams partial-but-valid TLS
 * bytes that the parser keeps wanting more of. */
#define TLS_PLAINTEXT_BUFFER_MAX (1u * 1024u * 1024u)

static bool tls_decrypt_into_buffer(http_connection_t *conn)
{
    for (;;) {
        size_t headroom = conn->read_buffer_size - conn->read_buffer_len;

        if (headroom == 0) {
            if (UNEXPECTED(conn->read_buffer_size >= TLS_PLAINTEXT_BUFFER_MAX)) {
                http_logf_warn(conn->log_state,
                    "tls.plaintext_buffer.exceeded cap_bytes=%u",
                    (unsigned)TLS_PLAINTEXT_BUFFER_MAX);
                return false;
            }

            size_t new_size = conn->read_buffer_size * 2;

            if (new_size > TLS_PLAINTEXT_BUFFER_MAX) {
                new_size = TLS_PLAINTEXT_BUFFER_MAX;
            }

            conn->read_buffer = erealloc(conn->read_buffer, new_size);
            conn->read_buffer_size = new_size;
            headroom = conn->read_buffer_size - conn->read_buffer_len;
        }

        size_t produced = 0;
        const tls_io_result_t rc = tls_read_plaintext(
            conn->tls,
            conn->read_buffer + conn->read_buffer_len,
            headroom,
            &produced);

        if (rc == TLS_IO_OK) {
            conn->read_buffer_len += produced;
            http_server_on_tls_io(conn->counters, produced, 0, 0, 0);

            if (produced == 0) {
                return true;
            }

            continue;
        }

        if (rc == TLS_IO_WANT_READ || rc == TLS_IO_WANT_WRITE) {
            return true;
        }

        if (rc == TLS_IO_CLOSED) {
            conn->state = CONN_STATE_CLOSING;
            return true;
        }

        return false;
    }
}

/* Feed conn->read_buffer into the protocol strategy and return:
 *    1 — need more data;
 *    0 — handler dispatched (parser reached on_message_complete);
 *   -1 — parse error or strategy missing. */
static int tls_feed_parser_step(http_connection_t *conn)
{
    if (UNEXPECTED(!conn->protocol_detected)) {
        if (!detect_and_assign_protocol(conn)) {
            return 1;
        }
    }

    if (UNEXPECTED(!conn->strategy || !conn->strategy->feed)) {
        return -1;
    }

    size_t consumed = 0;
    const int result = conn->strategy->feed(
        conn->strategy, conn, conn->read_buffer, conn->read_buffer_len, &consumed);

    if (result < 0) {
        return -1;
    }

    if (consumed < conn->read_buffer_len) {
        const size_t remaining = conn->read_buffer_len - consumed;
        memmove(conn->read_buffer, conn->read_buffer + consumed, remaining);
        conn->read_buffer_len = remaining;
    } else {
        conn->read_buffer_len = 0;
    }

    if (conn->parser && http_parser_is_complete(conn->parser)) {
        return 0;
    }

    return 1;
}

/* Best-effort bidirectional close: queue our close_notify alert and
 * kick the async send. We deliberately do NOT wait for the peer's
 * close_notify (RFC 8446 §6.1 permits skipping the reply, and every
 * major HTTP client closes TCP immediately after their last response
 * read).
 *
 * Drain ordering: with the single-in-flight gate on SSL_write in
 * tls_drain (see the long comment there), application plaintext can
 * accumulate in plaintext_bio while a previous WRITE_EX is still in
 * flight — common on h1 pipelining over TLS, where N responses commit
 * before the first record ships. Calling SSL_shutdown before that
 * plaintext is encrypted would queue close_notify ahead of the unsent
 * application bytes; some peers (and our own bookkeeping) interpret
 * close_notify as "stream done" and drop everything after it. Defer
 * the shutdown until plaintext_bio is empty AND no cipher write is in
 * flight; tls_drain re-enters this function via the cipher_completion
 * → tls_advance_state chain after each record drains, so the close
 * eventually fires. */
static void tls_graceful_close(http_connection_t *conn)
{
    if (conn->tls == NULL) {
        return;
    }

    (void)tls_drain(conn);

    if (conn->tls_cipher_inflight != 0) {
        return;
    }

    if (BIO_ctrl_pending(conn->tls_plaintext_bio_app) > 0) {
        return;
    }

    (void)tls_shutdown_step(conn->tls);
    (void)tls_drain(conn);
}

/* Ship any pending alert bytes the SSL state machine queued during a
 * fatal error path. Best-effort: a dead socket means there is no peer
 * to alert, and CLOSING transition still happens. */
static void tls_flush_pending_alert(http_connection_t *conn)
{
    if (conn == NULL || conn->tls == NULL) {
        return;
    }

    tls_drain(conn);
}

/* When a libuv submission (read/write) fails in callback context, the
 * underlying ext/async layer signals it two ways at once:
 *   - returns NULL from the *_io_* fn,
 *   - throws an InputOutputException (or similar) into EG via
 *     async_throw_error.
 *
 * In coroutine context the exception is absorbed by the coroutine at
 * the next suspend point. The TLS read FSM has no coroutine — leaving
 * the exception in EG would propagate it up to the top of the PHP VM
 * as a fatal. This helper extracts the exception details for a single
 * structured E_NOTICE, clears EG, and returns; the caller is
 * responsible for flipping tls_write_error and transitioning to
 * CLOSING.
 *
 * One log line per failed submission keeps DDoS-shaped client churn
 * (peer-RST after every request) bounded — same volume policy as
 * tls_log_error. */
static void tls_absorb_io_submission_exception(const http_connection_t *conn,
                                               const char *op)
{
    if (EG(exception) == NULL) {
        return;
    }

    zend_object *exc = EG(exception);
    zval rv;
    zval *msg_zv = zend_read_property_ex(
        exc->ce, exc, ZSTR_KNOWN(ZEND_STR_MESSAGE), 1, &rv);
    const char *msg = "(no message)";

    if (msg_zv != NULL && Z_TYPE_P(msg_zv) == IS_STRING && Z_STRLEN_P(msg_zv) > 0) {
        msg = Z_STRVAL_P(msg_zv);
    }

    http_logf_warn(conn->log_state,
        "tls.submission.failed op=%s ex=%s msg=%s",
        op, ZSTR_VAL(exc->ce->name), msg);
    zend_clear_exception();
}

/* One structured WARN record per failed connection; called at teardown
 * only. No-op when the session never recorded an error. */
static void tls_log_error(const http_connection_t *conn, const char *context)
{
    if (conn == NULL || conn->tls == NULL) {
        return;
    }

    const tls_error_info_t *err = tls_session_last_error(conn->tls);

    if (err == NULL || err->op == TLS_OP_NONE) {
        return;
    }

    http_logf_warn(conn->log_state,
        "tls.session.failed op=%s context=%s reason=%s "
        "ssl_err=%d state=%d bytes_done=%zu",
        tls_op_name(err->op),
        context != NULL ? context : "(unknown)",
        err->reason[0] != '\0' ? err->reason : "(no detail)",
        err->ssl_err, (int)err->state_at_fail, err->bytes_done);
}

/* ------------------------------------------------------------------------
 * State machine body.
 * ------------------------------------------------------------------------ */

/* Run on first transition out of TLS_HANDSHAKE: emits handshake-complete
 * telemetry, kTLS probe, and (for ALPN-known protocols) pre-installs the
 * strategy so the first feed lands in mem_recv directly. */
static void tls_on_handshake_done(http_connection_t *conn,
                                  uint64_t handshake_start_ns)
{
    http_server_on_tls_handshake_done(
        conn->server,
        zend_hrtime() - handshake_start_ns,
        tls_session_was_resumed(conn->tls));

    http_server_on_tls_ktls(
        conn->server,
        tls_session_ktls_tx_active(conn->tls),
        tls_session_ktls_rx_active(conn->tls));

    if (conn->protocol_detected) {
        return;
    }

    http_protocol_type_t alpn_proto = HTTP_PROTOCOL_UNKNOWN;

    if (conn->tls->alpn_selected == TLS_ALPN_H2) {
        alpn_proto = HTTP_PROTOCOL_HTTP2;
    } else if (conn->tls->alpn_selected == TLS_ALPN_HTTP11) {
        alpn_proto = HTTP_PROTOCOL_HTTP1;
    }

    if (alpn_proto == HTTP_PROTOCOL_UNKNOWN) {
        return;
    }

    conn->protocol_type = alpn_proto;
    conn->strategy = alpn_proto == HTTP_PROTOCOL_HTTP2
        ? http_protocol_strategy_http2_create()
        : http_protocol_strategy_http1_create();

    if (conn->strategy != NULL) {
        conn->strategy->on_request_ready = http_connection_on_request_ready;
        conn->protocol_detected = true;
    }
}

/* Per-connection handshake start timestamp. Stored in conn->created_at_ns
 * already; use that as the start so we don't add a TLS-only field. */
static inline uint64_t tls_handshake_start_ns(const http_connection_t *conn)
{
    return conn->created_at_ns;
}

/* Drive the state machine. Never destroys the connection — callers
 * inspect conn->state on return and call tls_finalize_if_closing to
 * tear down when appropriate. This invariant lets callers safely
 * touch conn after advance returns; otherwise a path that destroys
 * mid-FSM would leave the caller dereferencing freed memory. */
static void tls_advance_state(http_connection_t *conn)
{
    for (;;) {
        if (UNEXPECTED(conn->tls == NULL)) {
            return;
        }

        if (UNEXPECTED(conn->tls_write_error)) {
            tls_log_error(conn, "fsm");
            conn->state = CONN_STATE_CLOSING;
            return;
        }

        if (conn->state == CONN_STATE_TLS_HANDSHAKE) {
            const tls_io_result_t step = tls_handshake_step(conn->tls);
            tls_drain(conn);

            if (UNEXPECTED(conn->tls_write_error)) {
                continue;   /* falls through to error branch above */
            }

            if (step == TLS_IO_OK) {
                tls_on_handshake_done(conn, tls_handshake_start_ns(conn));
                conn->state = CONN_STATE_READING_HEADERS;
                continue;   /* try to feed any buffered ciphertext */
            }

            if (step == TLS_IO_WANT_READ || step == TLS_IO_WANT_WRITE) {
                /* WANT_WRITE means OpenSSL has bytes in the wbio for
                 * us to send; the FSM send path already submitted
                 * them. Progress depends on an external event
                 * (peer reply or send completion). */
                return;
            }
            /* TLS_IO_ERROR / TLS_IO_CLOSED. */
            tls_log_error(conn, "handshake");
            http_server_on_tls_handshake_failed(conn->server);
            conn->state = CONN_STATE_CLOSING;
            return;
        }

        if (conn->state == CONN_STATE_CLOSING) {
            /* Best-effort close_notify; caller decides when to destroy. */
            tls_graceful_close(conn);
            return;
        }

        /* All other states (READING_HEADERS, READING_BODY, PROCESSING,
         * SENDING, KEEPALIVE_WAIT) drive the same decrypt → feed loop.
         * The handler-coroutine boundary is signalled by feed == 0
         * (parser hit on_message_complete), not by conn->state — the
         * dispatch path inside on_headers_complete flips state to
         * PROCESSING long before the body has finished streaming, and
         * we still need to keep reading body chunks while the handler
         * sits suspended on awaitBody. */
        if (UNEXPECTED(!tls_decrypt_into_buffer(conn))) {
            tls_log_error(conn, "decrypt");
            tls_flush_pending_alert(conn);
            conn->state = CONN_STATE_CLOSING;
            continue;
        }
        /* SSL_read may have produced post-handshake messages. */
        tls_drain(conn);

        if (conn->state == CONN_STATE_CLOSING) {
            continue;   /* peer close_notify */
        }

        if (conn->read_buffer_len == 0) {
            return;     /* need more ciphertext */
        }

        /* Pipelining gate: if a handler is already in flight for this
         * connection, stop feeding the parser. The next request's bytes
         * stay in conn->read_buffer until tls_resume_after_handler clears
         * the flag and re-enters the FSM. Without this, a multishot read
         * delivering more ciphertext while handler N is still draining
         * would synchronously dispatch handler N+1 — overlapping handler
         * coroutines on the same conn, single-flusher-deadlock in
         * tls_drain, and lost responses on the LAST request (065 flake). */
        if (conn->tls_awaiting_handler) {
            return;
        }

        const int feed = tls_feed_parser_step(conn);

        if (feed < 0) {
            /* See http_connection.c handle_read_completion: latch on
             * the first parse-error tick to avoid double-counting +
             * double-emitting on subsequent multishot deliveries. */
            if (conn->parse_error_handled) {
                tls_flush_pending_alert(conn);
                conn->state = CONN_STATE_CLOSING;
                return;
            }

            if (conn->current_request != NULL
                && conn->current_request->coroutine != NULL) {
                http_connection_cancel_handler_for_parse_error(conn);
                conn->tls_awaiting_handler = true;
                return;
            }

            if (conn->parser != NULL) {
                (void)http_connection_emit_parse_error(conn, conn->parser);
            }

            tls_flush_pending_alert(conn);
            conn->state = CONN_STATE_CLOSING;
            continue;
        }

        if (feed == 0) {
            /* Parser hit on_message_complete. If the handler was
             * already dispatched at on_headers_complete (request
             * bodies of any size), it owns the next state transition;
             * idle the FSM until dispose. If no handler was dispatched
             * (a header-only request that errors before dispatch),
             * the parse-error branch above would have caught it — by
             * the time we reach feed==0 cleanly, a handler is alive. */
            conn->tls_awaiting_handler = true;
            return;
        }
        /* feed == 1: parser still wants bytes. Caller re-arms read. */
        return;
    }
}

/* Caller-side teardown gate. Used after every public entry into the
 * FSM (read completion, send completion, post-handler resume): if the
 * state machine landed in CLOSING and there is no in-flight FSM send
 * keeping the heap buffer alive, tear the connection down. Returns
 * true if conn was destroyed (caller MUST stop touching conn). */
static bool tls_finalize_if_closing(http_connection_t *conn)
{
    if (conn->state != CONN_STATE_CLOSING) {
        return false;
    }

    if (conn->tls_cipher_inflight != 0) {
        /* Hold off until the in-flight zero-copy write has cleared
         * the BIO ring slot. The free_cb re-runs teardown via
         * destroy_pending. */
        return false;
    }

    http_connection_destroy(conn);
    return true;
}

/* ========================================================================
 * Public entry points (called from http_connection.c).
 * ======================================================================== */

/* Arm the TLS read FSM on a freshly created connection. The first
 * ciphertext chunk arriving from the peer fires tls_read_callback_fn,
 * which feeds the BIO and runs tls_advance_state from CONN_STATE_TLS_HANDSHAKE.
 *
 * Note: server-side TLS is passive — the peer (client) speaks first
 * with ClientHello. We don't initiate a handshake step here; we just
 * arm the read and let the state machine pull on each chunk. */
bool http_connection_tls_arm_read(http_connection_t *conn)
{
    if (UNEXPECTED(!tls_arm_one_shot_read(conn))) {
        http_connection_destroy(conn);
        return false;
    }

    return true;
}

/* Internal-API accessor; see http_connection_internal.h for the
 * destroy-deferral contract. Tiny wrapper so the cb struct layout
 * stays private to this TU. */
bool http_connection_tls_fsm_send_in_flight(const http_connection_t *conn)
{
    return conn->tls_cipher_inflight != 0;
}

/* Re-enter the FSM after a handler coroutine has finished its dispose.
 * Called from http_handler_coroutine_dispose when conn->tls != NULL.
 * Handler dispose has already flipped conn->state to either KEEPALIVE_WAIT
 * (response sent, ready for next) or CLOSING (non-keep-alive). */
void http_connection_tls_resume_after_handler(http_connection_t *conn)
{
    conn->tls_awaiting_handler = false;

    tls_advance_state(conn);

    if (tls_finalize_if_closing(conn)) {
        return;
    }

    if (!conn->tls_awaiting_handler) {
        if (UNEXPECTED(!tls_arm_one_shot_read(conn))) {
            conn->state = CONN_STATE_CLOSING;
            tls_advance_state(conn);
            (void)tls_finalize_if_closing(conn);
        }
    }
}
