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

/* Producer + drain (issue #23): tls_push (coroutine) → plaintext_bio →
 * tls_drain (scheduler) → SSL_write → WRITE_EX → tls_cipher_completion. */

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

/* Park coroutine until plaintext_bio has `need` bytes of write room. */
static bool tls_wait_space(http_connection_t *conn, const size_t need)
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

/* Scheduler-side drain. tls_draining folds sync-complete re-entry. */
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

        /* Gate SSL_write on cipher_inflight: a fresh BIO_write races
         * libuv's pending read of the in-flight slot ("bad record mac"). */
        if (conn->tls_cipher_inflight == 0) {
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

        if (conn->tls_cipher_inflight == 0) {
            if (UNEXPECTED(!tls_fsm_io_cb_attach(conn))) {
                ok = false;
                break;
            }

            char        *slot         = NULL;
            const size_t cipher_avail = tls_peek_cipher_out(conn->tls, &slot);

            if (cipher_avail > 0) {
                /* Slot stays stable: SSL_write gated on cipher_inflight; consume after completion. */
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
                    /* Sync-complete: free_cb cleared cipher_inflight already. */
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

/* Producer entry (coroutine-only). Chunks internally for len > ring_size. */
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

    /* zend_bailout firewall — topmost extension entry from libuv read
     * completion (io_pipe_read_cb → callbacks_notify → here). Catches
     * memory_limit OOM in tls_advance_state (nghttp2 callbacks, static
     * slurp, request/response init) and tls_arm_one_shot_read
     * (per-request ecalloc). Without this catch the bailout escapes
     * libuv → scheduler → worker abort. With catch, OOM kills only
     * this connection (write_timed_out, abort streams, deferred destroy).
     * Per-stream rollback is structurally impossible because nghttp2 /
     * HPACK / TLS state is shared. */
    volatile bool bailout = false;
    zend_try {

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
        } else if (UNEXPECTED(!tls_commit_cipher_in(conn->tls, (size_t)bytes_read))) {
            conn->state = CONN_STATE_CLOSING;
            tls_advance_state(conn);
            (void)tls_finalize_if_closing(conn);
        } else {
            http_server_on_tls_io(conn->counters, 0, 0, (size_t)bytes_read, 0);

            tls_advance_state(conn);

            if (!tls_finalize_if_closing(conn)) {
                if (!conn->tls_awaiting_handler) {
                    if (UNEXPECTED(!tls_arm_one_shot_read(conn))) {
                        conn->state = CONN_STATE_CLOSING;
                        tls_advance_state(conn);
                        (void)tls_finalize_if_closing(conn);
                    }
                }
            }
        }
    }

    } zend_catch {
        bailout = true;
    } zend_end_try();

    if (UNEXPECTED(bailout)) {
        conn->write_timed_out = true;

        fprintf(stderr,
                "[true-async-server] zend_bailout in tls read-cb: conn=%p — %s\n",
                (const void *)conn,
                PG(last_error_message) != NULL
                    ? ZSTR_VAL(PG(last_error_message)) : "(no PG message)");
        fflush(stderr);

        ZEND_ASYNC_SHUTDOWN();
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

    if (UNEXPECTED(conn->destroy_pending) && conn->handler_refcount == 0) {
        conn->destroy_pending = false;
        http_connection_destroy(conn);
        return;
    }

    tls_advance_state(conn);

    if (tls_finalize_if_closing(conn)) {
        return;
    }

    /* Wake h2 emit — plaintext BIO has room now (no-op on non-h2). */
    {
        extern void http2_conn_notify_emit(http_connection_t *);
        http2_conn_notify_emit(conn);
    }

    /* Static FSM observer: signals wbio drained so the next file chunk
     * can encrypt without SSL_ERROR_WANT_WRITE. */
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

/* Defer SSL_shutdown until plaintext_bio drained + no cipher in flight —
 * close_notify must not jump ahead of unsent app data (h1 pipelining). */
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

/* FSM has no coroutine to absorb EG exception — log + clear; caller flips
 * tls_write_error and transitions to CLOSING. */
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

/* =====================================================================
 * kTLS connection path (issue #30 Phase 2).
 *
 * Lives next to the memory-BIO FSM above so all TLS state-machine logic
 * stays in one TU. Activated when the connection's tls_session was built
 * by tls_session_new_socket(); recognised by conn->ktls_mode.
 *
 * Differences from the memory-BIO FSM:
 *
 *   - No tls_feed_ciphertext / tls_drain_ciphertext pump. OpenSSL talks
 *     to the socket fd directly through BIO_new_socket, so SSL_read and
 *     SSL_write just call recvmsg / sendmsg — the kernel decrypts /
 *     encrypts in place when kTLS is engaged on the fd.
 *
 *   - I/O readiness is signalled by a zend_async_poll_event_t on the
 *     raw fd (ASYNC_READABLE / ASYNC_WRITABLE) instead of a libuv
 *     ZEND_ASYNC_IO_READ multishot.
 *
 *   - tls_drain() / tls_cipher_inflight / tls_zc_write_done_cb plumbing
 *     is unused — there is no ciphertext queue to coalesce.
 *
 * The handshake driver in this commit only handles CONN_STATE_TLS_HANDSHAKE.
 * Once it transitions to CONN_STATE_READING_HEADERS the data path is TBD
 * and the connection idles until read_timeout fires it. The next commit
 * on this branch fills in the data path.
 * ===================================================================== */

typedef struct {
    zend_async_event_callback_t  base;
    http_connection_t           *conn;
} ktls_poll_cb_t;

static void ktls_poll_cb_dispose(zend_async_event_callback_t *cb,
                                 zend_async_event_t *event)
{
    (void)event;
    efree(cb);
}

/* Re-arm the poll event for the requested direction. Stops + restarts
 * with a new `events` mask — same recipe as ext/curl's curl_async.c. */
static bool ktls_arm_for(http_connection_t *conn, const async_poll_event events)
{
    if (conn->ktls_poll == NULL) {
        return false;
    }

    conn->ktls_poll->base.stop(&conn->ktls_poll->base);
    conn->ktls_poll->events = events;
    ZEND_ASYNC_EVENT_CLR_CLOSED(&conn->ktls_poll->base);
    return conn->ktls_poll->base.start(&conn->ktls_poll->base);
}

/* FSM step for a kTLS connection. Loops while we can make synchronous
 * progress; returns once we need to wait for socket readiness (poll
 * callback re-enters here) or the connection has transitioned to
 * CLOSING (caller finalises). */
static void ktls_advance(http_connection_t *conn)
{
    while (conn->state != CONN_STATE_CLOSING) {
        if (conn->state == CONN_STATE_TLS_HANDSHAKE) {
            const tls_io_result_t step = tls_handshake_step(conn->tls);

            if (step == TLS_IO_OK) {
                tls_on_handshake_done(conn, tls_handshake_start_ns(conn));
                conn->state = CONN_STATE_READING_HEADERS;
                /* Data path lands in the next commit — for now arm a
                 * read so the FSM at least sleeps on readability when
                 * the data path arrives. */
                if (!ktls_arm_for(conn, ASYNC_READABLE)) {
                    conn->state = CONN_STATE_CLOSING;
                    continue;
                }
                return;
            }

            if (step == TLS_IO_WANT_READ) {
                if (!ktls_arm_for(conn, ASYNC_READABLE)) {
                    conn->state = CONN_STATE_CLOSING;
                    continue;
                }
                return;
            }

            if (step == TLS_IO_WANT_WRITE) {
                if (!ktls_arm_for(conn, ASYNC_WRITABLE)) {
                    conn->state = CONN_STATE_CLOSING;
                    continue;
                }
                return;
            }

            /* TLS_IO_ERROR / TLS_IO_CLOSED. */
            tls_log_error(conn, "ktls-handshake");
            http_server_on_tls_handshake_failed(conn->server);
            conn->state = CONN_STATE_CLOSING;
            continue;
        }

        /* Data states (READING_HEADERS, READING_BODY, ...) — TODO next
         * commit. Returning here without arming I/O parks the
         * connection; deadline_tick eventually force-closes it. */
        return;
    }

    (void)tls_finalize_if_closing(conn);
}

static void ktls_poll_callback(zend_async_event_t *event,
                               zend_async_event_callback_t *callback,
                               void *result,
                               zend_object *exception)
{
    (void)event;
    (void)result;

    ktls_poll_cb_t *cb = (ktls_poll_cb_t *)callback;
    http_connection_t *conn = cb->conn;

    if (UNEXPECTED(conn == NULL || conn->state == CONN_STATE_CLOSING)) {
        return;
    }

    if (UNEXPECTED(exception != NULL)) {
        conn->state = CONN_STATE_CLOSING;
        (void)tls_finalize_if_closing(conn);
        return;
    }

    /* Bailout firewall: SSL_do_handshake → callbacks may emalloc. */
    volatile bool bailout = false;
    zend_try {
        ktls_advance(conn);
    } zend_catch {
        bailout = true;
    } zend_end_try();

    if (UNEXPECTED(bailout)) {
        conn->tls_write_error = true;
        conn->state = CONN_STATE_CLOSING;
        (void)tls_finalize_if_closing(conn);
    }
}

bool http_connection_ktls_arm_handshake(http_connection_t *conn,
                                        const php_socket_t socket_fd)
{
    if (UNEXPECTED(conn == NULL || conn->tls == NULL || socket_fd < 0)) {
        return false;
    }

    if (UNEXPECTED(!tls_session_is_socket_bio(conn->tls))) {
        /* Caller is supposed to build a socket-BIO session before
         * arming the kTLS path. Refuse to attach to a memory-BIO
         * session — would deadlock immediately on missing pump. */
        return false;
    }

    conn->ktls_fd   = socket_fd;
    conn->ktls_poll = ZEND_ASYNC_NEW_SOCKET_EVENT(socket_fd,
                          ASYNC_READABLE | ASYNC_WRITABLE);

    if (conn->ktls_poll == NULL) {
        return false;
    }

    ktls_poll_cb_t *cb = (ktls_poll_cb_t *)
        ZEND_ASYNC_EVENT_CALLBACK_EX(ktls_poll_callback, sizeof(*cb));

    if (UNEXPECTED(cb == NULL)) {
        conn->ktls_poll->base.dispose(&conn->ktls_poll->base);
        conn->ktls_poll = NULL;
        return false;
    }

    cb->base.dispose = ktls_poll_cb_dispose;
    cb->conn         = conn;

    if (UNEXPECTED(!conn->ktls_poll->base.add_callback(
                       &conn->ktls_poll->base, &cb->base))) {
        ktls_poll_cb_dispose(&cb->base, NULL);
        conn->ktls_poll->base.dispose(&conn->ktls_poll->base);
        conn->ktls_poll = NULL;
        return false;
    }

    conn->ktls_poll_cb = &cb->base;

    if (UNEXPECTED(!conn->ktls_poll->base.start(&conn->ktls_poll->base))) {
        /* Callback is now owned by the event; destroy path will dispose
         * the event which calls our dispose on the callback. */
        return false;
    }

    /* First synchronous tick — SSL_do_handshake's initial call doesn't
     * need any peer bytes for the typical TLS 1.3 server-side state, so
     * we try once before parking on readiness. Result is one of:
     *   - WANT_READ  → already armed READABLE, return
     *   - WANT_WRITE → re-arm WRITABLE, return
     *   - OK         → transition to data path, arm READABLE
     *   - ERROR      → state = CLOSING (caller finalises). */
    ktls_advance(conn);
    return true;
}
