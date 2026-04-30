/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+

  TLS connection path — event-driven read FSM and producer-side flusher.

  Two coexisting writers share the cipher BIO; coordination is via the
  tls_flushing flag rather than a coroutine boundary:

    1. Producer flusher — runs from the handler coroutine. The handler
       calls http_connection_send → tls_push_and_maybe_flush, which
       enqueues plaintext into the BIO pair, optionally takes the
       flusher role, encrypts via SSL_write, and ships ciphertext via
       the suspending http_connection_send_raw. Suspension is fine
       here: a coroutine is alive.

    2. Read FSM — driven by a persistent read callback on io->event.
       Each ciphertext chunk fires tls_read_callback_fn → tls_advance_state,
       which steps the SSL state machine (handshake, decrypt, parse,
       dispatch). Bytes the FSM produces (handshake records, post-
       handshake messages, alerts, close_notify) cannot use the
       suspending send path — the FSM has no coroutine to suspend on.
       They are siphoned into a private heap buffer and shipped via a
       non-blocking ZEND_ASYNC_IO_WRITE; completion re-enters the FSM.

  When both writers want the cipher BIO at once, the producer flusher
  wins: tls_flushing serialises the write, and any FSM-produced bytes
  are picked up by the flusher's next BIO_nread0 iteration. The FSM's
  async send path skips submission while tls_flushing is held.
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

/* ------------------------------------------------------------------------
 * Forward declarations — both subsystems reference each other's helpers.
 * ------------------------------------------------------------------------ */
static bool tls_drain_ring_to_socket(http_connection_t *conn);
static bool tls_ssl_write_and_drain(http_connection_t *conn,
                                    const char *data, size_t len);
static inline void trigger_tls_drain_event(http_connection_t *conn);
static bool await_tls_drain_event(http_connection_t *conn);
static bool tls_drain(http_connection_t *conn);

static bool tls_decrypt_into_buffer(http_connection_t *conn);
static int  tls_feed_parser_step(http_connection_t *conn);
static void tls_graceful_close(http_connection_t *conn);
static void tls_flush_pending_alert(http_connection_t *conn);
static void tls_log_error(const http_connection_t *conn, const char *context);
static void tls_absorb_io_submission_exception(const http_connection_t *conn,
                                               const char *op);

static void tls_fsm_send_kick(http_connection_t *conn);
static bool tls_fsm_io_cb_attach(http_connection_t *conn);
static void tls_fsm_io_callback_fn(zend_async_event_t *event,
                                   zend_async_event_callback_t *callback,
                                   void *result, zend_object *exception);
static void tls_fsm_io_callback_dispose(zend_async_event_callback_t *callback,
                                        zend_async_event_t *event);

static bool tls_arm_one_shot_read(http_connection_t *conn);

static void tls_advance_state(http_connection_t *conn);
static bool tls_finalize_if_closing(http_connection_t *conn);

/* ========================================================================
 * Producer-side flusher (handler coroutine path)
 *
 * Unchanged from the pre-refactor design — still runs only from a handler
 * coroutine and still uses the suspending http_connection_send_raw. The
 * FSM never enters this path; FSM-produced ciphertext goes through the
 * async send path further down.
 * ======================================================================== */

/* Zero-copy drain helper: pulls every queued ciphertext byte out of
 * the BIO pair via tls_peek_cipher_out (direct pointer into the ring,
 * no memcpy) and hands it straight to the socket via
 * http_connection_send_raw. The ring never wraps a peeked region, so
 * a full ring takes two iterations in the worst case. */
static bool tls_drain_ring_to_socket(http_connection_t *conn)
{
    for (;;) {
        char *slot = NULL;
        const size_t avail = tls_peek_cipher_out(conn->tls, &slot);
        if (avail == 0 || slot == NULL) {
            return true;
        }
        if (!http_connection_send_raw(conn, slot, avail)) {
            return false;
        }
        if (!tls_consume_cipher_out(conn->tls, avail)) {
            return false;
        }
        http_server_on_tls_io(conn->counters, 0, 0, 0, avail);
    }
}

/* Encrypt @p len bytes of plaintext through the session and ship the
 * resulting ciphertext out the socket. Must run with conn->tls_flushing
 * == true — callers without that invariant race with each other on
 * SSL_write. */
static bool tls_ssl_write_and_drain(http_connection_t *conn,
                                    const char *data, size_t len)
{
    size_t offset = 0;
    while (offset < len) {
        size_t written = 0;
        const tls_io_result_t rc = tls_write_plaintext(
            conn->tls, data + offset, len - offset, &written);
        if (rc == TLS_IO_OK) {
            offset += written;
            http_server_on_tls_io(conn->counters, 0, written, 0, 0);
        } else if (rc != TLS_IO_WANT_READ && rc != TLS_IO_WANT_WRITE) {
            return false;
        }
        if (!tls_drain_ring_to_socket(conn)) {
            return false;
        }
    }
    return true;
}

/* Wake any producer suspended on a full plaintext ring. */
static inline void trigger_tls_drain_event(http_connection_t *conn)
{
    if (conn->tls_drain_event != NULL) {
        conn->tls_drain_event->trigger(conn->tls_drain_event);
    }
}

/* Suspend the calling coroutine until the flusher frees space in the
 * plaintext ring. Returns false on cancellation / scope stop. */
static bool await_tls_drain_event(http_connection_t *conn)
{
    if (conn->tls_drain_event == NULL) {
        conn->tls_drain_event = ZEND_ASYNC_NEW_TRIGGER_EVENT();
        if (conn->tls_drain_event == NULL) {
            return false;
        }
    }

    zend_coroutine_t *const co = ZEND_ASYNC_CURRENT_COROUTINE;
    if (ZEND_ASYNC_WAKER_NEW(co) == NULL) {
        return false;
    }

    zend_async_resume_when(co, &conn->tls_drain_event->base, false,
                           zend_async_waker_callback_resolve, NULL);

    ZEND_ASYNC_SUSPEND();
    zend_async_waker_clean(co);

    return EG(exception) == NULL;
}

/* Drain the plaintext ring under the single-writer invariant.
 *
 * Contract:
 *   - If tls_flushing is already true, another coroutine owns the
 *     flush. Return — that flusher will pick up whatever is in the
 *     ring before it clears the flag.
 *   - Otherwise take the flusher role, loop over every contiguous
 *     segment BIO_nread0 hands us, encrypt it, and drain residual
 *     ciphertext.
 *   - Any SSL_write or socket failure sets tls_write_error (sticky);
 *     future callers see false without touching SSL state. */
static bool tls_drain(http_connection_t *conn)
{
    if (conn->tls == NULL || conn->tls_write_error) {
        return false;
    }
    if (conn->tls_flushing) {
        return true;
    }

    conn->tls_flushing = true;
    bool ok = true;

    while (ok) {
        char *plain = NULL;
        const int avail = BIO_nread0(conn->tls_plaintext_bio_app, &plain);
        if (avail <= 0 || plain == NULL) {
            break;
        }

        if (!tls_ssl_write_and_drain(conn, plain, (size_t)avail)) {
            ok = false;
            break;
        }

        /* Advance past the bytes we just encrypted. BIO_nread normally
         * consumes everything in one call; the loop absorbs the
         * ring-wrap edge case. */
        int remaining = avail;
        while (remaining > 0) {
            char *dummy = NULL;
            const int consumed = BIO_nread(conn->tls_plaintext_bio_app,
                                           &dummy, remaining);
            if (consumed <= 0) {
                ok = false;
                break;
            }
            remaining -= consumed;
        }
        if (!ok) {
            break;
        }

        trigger_tls_drain_event(conn);
    }

    /* Ship residual ciphertext — post-handshake messages emitted by
     * SSL_read (TLS 1.3 NewSessionTicket, KeyUpdate), shutdown alerts
     * from the session state machine, anything the BIO still holds. */
    if (ok) {
        ok = tls_drain_ring_to_socket(conn);
    }

    conn->tls_flushing = false;
    if (!ok) {
        conn->tls_write_error = true;
    }
    return ok;
}

/* Producer entry point. Pushes @p len plaintext bytes into the ring
 * (waiting on tls_drain_event if full) and then either takes the
 * flusher role or returns immediately if one is already held. */
bool tls_push_and_maybe_flush(http_connection_t *conn,
                              const char *data, size_t len)
{
    if (conn->tls_write_error) {
        return false;
    }

    size_t off = 0;
    while (off < len) {
        const size_t chunk = len - off;
        const int to_write = chunk > (size_t)INT_MAX ? INT_MAX : (int)chunk;
        const int n = BIO_write(conn->tls_plaintext_bio, data + off, to_write);
        if (n > 0) {
            off += (size_t)n;
            continue;
        }
        if (!BIO_should_retry(conn->tls_plaintext_bio)) {
            conn->tls_write_error = true;
            return false;
        }
        /* Ring full mid-push. Take the flusher role if free; otherwise
         * park on drain_event until another flusher makes room. */
        if (!conn->tls_flushing) {
            if (!tls_drain(conn)) {
                return false;
            }
        } else {
            if (!await_tls_drain_event(conn)) {
                return false;
            }
        }
        if (conn->tls == NULL || conn->tls_write_error) {
            return false;
        }
    }

    return tls_drain(conn);
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
 * Coordination with the producer flusher: while tls_flushing is held
 * the FSM defers submission and lets the flusher's BIO_nread0 loop
 * pick up the bytes. The flusher already drains residual ciphertext
 * via tls_drain_ring_to_socket on every loop iteration.
 * ======================================================================== */

/* Unified callback. Both the read FSM and the FSM async-send share a
 * single callback on io->event so the io subsystem fires us at most
 * once per NOTIFY. Holding two separate callbacks here is unsafe:
 * disposing the read req from inside the read branch frees its slot
 * back to zend_mm; if the FSM then submits a fresh write req that
 * gets the same recycled address, the stored result pointer in the
 * still-iterating notify loop falsely matches the second callback's
 * active_req on the next iteration. With one callback the dispatch
 * happens once and the recycled-address path stays harmless. */
struct _tls_fsm_send_cb {
    zend_async_event_callback_t  base;
    http_connection_t           *conn;
    zend_async_io_req_t         *read_req;     /* outstanding read, NULL when idle */
    zend_async_io_req_t         *write_req;    /* outstanding FSM-send write, NULL when idle */
    char                        *write_buf;    /* heap-owned bytes underlying write_req */
    size_t                       write_buf_len;
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

    /* Dispatch by req identity. read_req and write_req are distinct
     * pointers while both live; once one is disposed and a fresh
     * allocation reuses its slot, that fresh allocation lives on the
     * other field — a stale match against the just-fired result is
     * impossible because notify carries a single result pointer per
     * fire. */
    const bool is_read  = (cb->read_req  != NULL && result == cb->read_req);
    const bool is_write = (cb->write_req != NULL && result == cb->write_req);
    if (!is_read && !is_write) {
        return;
    }

    if (is_write) {
        zend_async_io_req_t *req = cb->write_req;
        const ssize_t transferred = req->transferred;
        const bool err = (exception != NULL) || (req->exception != NULL);
        if (UNEXPECTED(req->exception != NULL)) {
            OBJ_RELEASE(req->exception);
            req->exception = NULL;
        }
        cb->write_req = NULL;
        req->dispose(req);

        if (cb->write_buf != NULL) {
            efree(cb->write_buf);
            cb->write_buf = NULL;
            cb->write_buf_len = 0;
        }
        if (UNEXPECTED(err || transferred < 0)) {
            conn->tls_write_error = true;
        }

        if (UNEXPECTED(conn->destroy_pending) && conn->handler_refcount == 0) {
            conn->destroy_pending = false;
            http_connection_destroy(conn);
            return;
        }

        tls_advance_state(conn);
        (void)tls_finalize_if_closing(conn);
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
    cb->write_req       = NULL;
    cb->write_buf       = NULL;
    cb->write_buf_len   = 0;

    if (UNEXPECTED(!conn->io->event.add_callback(&conn->io->event, &cb->base))) {
        tls_fsm_io_callback_dispose(&cb->base, NULL);
        return false;
    }
    conn->tls_fsm_send_cb = cb;
    return true;
}

/* Pull every byte of pending ciphertext out of the BIO pair into a
 * single heap buffer and submit one non-blocking write. On sync-
 * complete we consume + free in-line and may re-enter to drain any
 * bytes the just-completed write opened up. The async path lets the
 * completion callback take over.
 *
 * No-op when:
 *   - no bytes pending;
 *   - tls_flushing is held (producer flusher will ship our bytes);
 *   - a previous FSM submission is still in flight;
 *   - tls_write_error is sticky. */
static void tls_fsm_send_kick(http_connection_t *conn)
{
    if (conn->tls == NULL || conn->tls_write_error) {
        return;
    }
    if (conn->tls_flushing) {
        return;
    }
    if (UNEXPECTED(!tls_fsm_io_cb_attach(conn))) {
        conn->tls_write_error = true;
        return;
    }
    tls_fsm_io_cb_t *cb = conn->tls_fsm_send_cb;
    if (cb->write_req != NULL) {
        return;   /* an FSM write is already in flight; will re-kick on completion */
    }

    /* Siphon the entire BIO output ring into one heap buffer so the
     * write is atomic from the FSM's view and BIO_nread is consumed
     * in the same step. The buffer rarely exceeds one TLS record
     * (~16 KiB) for FSM-produced bytes. */
    char  *buf       = NULL;
    size_t total     = 0;
    size_t buf_cap   = 0;
    bool   bio_error = false;

    for (;;) {
        char *slot = NULL;
        const size_t avail = tls_peek_cipher_out(conn->tls, &slot);
        if (avail == 0 || slot == NULL) {
            break;
        }
        if (total + avail > buf_cap) {
            buf_cap = total + avail;
            buf = erealloc(buf, buf_cap);
        }
        memcpy(buf + total, slot, avail);
        if (!tls_consume_cipher_out(conn->tls, avail)) {
            bio_error = true;
            break;
        }
        total += avail;
    }

    if (UNEXPECTED(bio_error)) {
        if (buf != NULL) {
            efree(buf);
        }
        conn->tls_write_error = true;
        return;
    }
    if (total == 0) {
        if (buf != NULL) {
            efree(buf);
        }
        return;
    }

    zend_async_io_req_t *req = ZEND_ASYNC_IO_WRITE(conn->io, buf, total);
    if (UNEXPECTED(req == NULL)) {
        tls_absorb_io_submission_exception(conn, "write");
        efree(buf);
        conn->tls_write_error = true;
        return;
    }

    /* Sync-complete fast path — consume in line, recurse to drain any
     * bytes the SSL state machine added while we were here. */
    if (req->completed) {
        const bool err = (req->exception != NULL);
        const ssize_t transferred = req->transferred;
        if (req->exception != NULL) {
            OBJ_RELEASE(req->exception);
            req->exception = NULL;
        }
        req->dispose(req);
        efree(buf);

        if (UNEXPECTED(err || transferred < (ssize_t)total)) {
            conn->tls_write_error = true;
            return;
        }
        http_server_on_tls_io(conn->counters, 0, 0, 0, (size_t)transferred);

        /* SSL may have produced more bytes since we started; re-kick. */
        tls_fsm_send_kick(conn);
        return;
    }

    /* Async path: hand the buffer to the unified io callback. */
    cb->write_req     = req;
    cb->write_buf     = buf;
    cb->write_buf_len = total;
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
    tls_fsm_send_kick(conn);
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
    tls_fsm_io_cb_t *const cb = conn->tls_fsm_send_cb;
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
 * read). */
static void tls_graceful_close(http_connection_t *conn)
{
    if (conn->tls == NULL) {
        return;
    }
    (void)tls_shutdown_step(conn->tls);
    tls_fsm_send_kick(conn);
}

/* Ship any pending alert bytes the SSL state machine queued during a
 * fatal error path. Best-effort: a dead socket means there is no peer
 * to alert, and CLOSING transition still happens. */
static void tls_flush_pending_alert(http_connection_t *conn)
{
    if (conn == NULL || conn->tls == NULL) {
        return;
    }
    tls_fsm_send_kick(conn);
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
    zend_object *const exc = EG(exception);
    zval rv;
    zval *const msg_zv = zend_read_property_ex(
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
    const tls_error_info_t *const err = tls_session_last_error(conn->tls);
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
            tls_fsm_send_kick(conn);
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
                 * us to send; tls_fsm_send_kick already submitted
                 * them. Progress now depends on an external event
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
        tls_fsm_send_kick(conn);

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
    if (conn->tls_fsm_send_cb != NULL
        && conn->tls_fsm_send_cb->write_req != NULL) {
        /* Hold off until close_notify (or whatever the FSM queued) has
         * left the wire. The send-completion callback re-runs the
         * teardown via destroy_pending. */
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
    return conn->tls_fsm_send_cb != NULL
        && conn->tls_fsm_send_cb->write_req != NULL;
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
