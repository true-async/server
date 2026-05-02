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
#include "zend_exceptions.h"
#include "Zend/zend_async_API.h"
#include "Zend/zend_hrtime.h"
#include "php_http_server.h"
#include "http1/http_parser.h"
#include "http_connection.h"
#include "http_connection_internal.h"
#include "conn_arena.h"
#include "log/http_log.h"           /* http_logf_debug for absorbed-exception sites */
#include "http_protocol_strategy.h"

/* php_network.h (pulled in via http_connection.h) supplies socket
 * types and closesocket on both POSIX and Windows. No direct POSIX
 * headers are needed — the async reactor owns the socket lifecycle. */
#include <limits.h>
#include <stdint.h>

/* MSG_NOSIGNAL is a Linux extension. On BSD/macOS SO_NOSIGPIPE is the
 * equivalent; on Windows SIGPIPE does not exist. Providing a no-op macro
 * keeps the call sites portable. */
#ifndef MSG_NOSIGNAL
# define MSG_NOSIGNAL 0
#endif

#define DEFAULT_READ_BUFFER_SIZE 8192

extern zval* http_request_create_from_parsed(http_request_t *req);
extern zend_string *http_response_format(zend_object *obj);
extern void http_response_set_protocol_version(zend_object *obj, const char *version);
extern bool http_response_is_closed(zend_object *obj);
extern bool http_response_is_committed(zend_object *obj);
extern void http_response_set_committed(zend_object *obj);
extern void http_response_reset_to_error(zend_object *obj, int status_code, const char *message);
extern void http_response_force_connection_close(zend_object *obj);

/* Persistent read callback attached to conn->io->event. Its lifetime
 * matches the connection — not per-read — so the callback struct holds
 * the conn back-reference and is only disposed from
 * http_connection_destroy (or its own dispose hook if ref_count hits 0
 * earlier because the io handle was closed by the reactor). */
struct _http_connection_read_cb {
    zend_async_event_callback_t base;
    http_connection_t         *conn;
    zend_async_io_req_t       *active_req;  /* current outstanding read req, NULL when idle */
};

/* cancel-for-parse-error is declared in http_connection_internal.h —
 * it's also called from http_connection_tls.c. */
static bool http_connection_handle_read_completion(http_connection_t *conn,
                                                   bool *should_destroy_out);
static void http_connection_dispatch_request(http_connection_t *conn, http_request_t *req);
static void http_handler_coroutine_entry(void);
static void http_handler_coroutine_dispose(zend_coroutine_t *coroutine);
static void http_connection_alloc_cb(zend_async_io_t *io, size_t suggested, zend_async_buf_t *out);
static void http_connection_read_callback_fn(
    zend_async_event_t *event,
    zend_async_event_callback_t *callback,
    void *result,
    zend_object *exception);
static void http_connection_read_callback_dispose(
    zend_async_event_callback_t *callback,
    zend_async_event_t *event);
static void http_write_timer_dispose(http_connection_t *conn);

/* {{{ http_connection_on_request_ready
 *
 * Dispatch callback. Fired synchronously from deep inside
 * llhttp_execute (via parser->dispatch_cb, set up in http_parser_attach)
 * once on_headers_complete has populated method/uri/headers on req.
 * The body has NOT yet been fully received at this point — parsing
 * continues after this call returns, and on_body keeps writing into
 * the same req struct. The handler coroutine we spawn here can either
 * call $req->awaitBody() to park on body_event, or work with the
 * partial body directly.
 */
/* Cross-TU: TLS coroutine wires this on its ALPN fast-path strategy. */
void http_connection_on_request_ready(http_connection_t *conn, http_request_t *req)
{
    conn->request_ready = true;

    /* Capture HTTP version + keep-alive BEFORE handing off — the handler
     * coroutine uses conn->http_version / conn->keep_alive after
     * dispatch runs. major/minor are 0..9 in HTTP/1.x, written directly
     * to skip libc format_converter on the hot path. */
    conn->http_version[0] = '0' + (char)req->http_major;
    conn->http_version[1] = '.';
    conn->http_version[2] = '0' + (char)req->http_minor;
    conn->http_version[3] = '\0';
    conn->keep_alive = req->keep_alive;

    /* CoDel enqueue point: parser finished, request about to be
     * dispatched. Stamping on req (not conn) so concurrent streams
     * (HTTP/2) don't overwrite each other. Skipped entirely when no
     * consumer (CoDel/telemetry) is active — see step 4.1 in PLAN.md. */
    if (http_server_sample_stamps_enabled(conn->view)) {
        req->enqueue_ns = zend_hrtime();
    }

    http_connection_dispatch_request(conn, req);
}
/* }}} */

/* {{{ http_connection_create
 *
 * Wraps an accepted socket fd in a TrueAsync TCP IO handle. The handle takes
 * ownership of the fd: libuv's uv_tcp_open() attaches the native socket to
 * a uv_tcp_t and the final ZEND_ASYNC_IO_CLOSE call closes it asynchronously.
 *
 * libuv guarantees non-blocking mode on accepted TCP fds, so we don't have
 * to call fcntl(O_NONBLOCK) ourselves. TCP_NODELAY, however, is only set on
 * the listen socket inside libuv_socket_listen — we enable it per-client
 * via ZEND_ASYNC_IO_SET_OPTION for low-latency responses.
 */
http_connection_t *http_connection_create(const php_socket_t socket_fd, zend_async_scope_t *parent_scope,
                                          struct http_server_object *server)
{
    http_connection_t *conn;
    if (server != NULL) {
        conn = conn_arena_alloc(http_server_arena(server));
    } else {
        /* Server-less test path. The slot is just an ecalloc'd
         * standalone allocation, not in any arena's alive list, and
         * destroy must NOT call conn_arena_free. */
        conn = ecalloc(1, sizeof(http_connection_t));
    }
    if (UNEXPECTED(conn == NULL)) {
        closesocket(socket_fd);
        return NULL;
    }
    conn->server = server;

    conn->io = ZEND_ASYNC_IO_CREATE(
        (zend_file_descriptor_t)socket_fd,
        ZEND_ASYNC_IO_TYPE_TCP,
        ZEND_ASYNC_IO_READABLE | ZEND_ASYNC_IO_WRITABLE
    );
    if (!conn->io) {
        if (server != NULL) {
            conn_arena_free(http_server_arena(server), conn);
        } else {
            efree(conn);
        }
        closesocket(socket_fd);
        return NULL;
    }

    ZEND_ASYNC_IO_SET_OPTION(conn->io, ZEND_ASYNC_SOCKET_OPT_NODELAY, 1);

    /* Multishot read: a single submitted ZEND_ASYNC_IO_READ stays armed after
     * each chunk is delivered, instead of completing + requiring a new req per
     * chunk. Saves one req allocation and one uv_read_start per chunk for
     * body streaming and keep-alive pipelines. The req is only disposed on
     * a terminal event (EOF/error) or when we explicitly stop the reader at
     * request hand-off. */
    ZEND_ASYNC_IO_SET_MULTISHOT(conn->io);

    conn->state = CONN_STATE_READING_HEADERS;
    conn->protocol_type = HTTP_PROTOCOL_UNKNOWN;
    conn->strategy = NULL;
    conn->protocol_detected = false;

    conn->parser = NULL;

    conn->read_buffer_size = DEFAULT_READ_BUFFER_SIZE;
    conn->read_buffer = emalloc(conn->read_buffer_size);
    conn->read_buffer_len = 0;

    /* Wire up the per-chunk allocator: lets multishot stay armed across
     * back-to-back keep-alive requests and pipelined tails without a
     * uv_read_stop/start cycle. The reactor calls back into us with the
     * sliding offset on every chunk. */
    conn->io->alloc_cb  = http_connection_alloc_cb;
    conn->io->user_data = conn;

    conn->keep_alive = true;
    conn->headers_complete = false;
    conn->body_complete = false;
    conn->request_ready = false;

    /* Default to dummy counters / view; spawn() rebinds these to the
     * server's slices once the owning server is known. */
    conn->counters  = &http_server_counters_dummy;
    conn->view      = &http_server_view_default;
    conn->log_state = &http_log_state_default;

    return conn;
}
/* }}} */

/* {{{ http_connection_destroy */
void http_connection_destroy(http_connection_t *conn)
{
    if (!conn) return;

    /* Defer the actual free if a handler coroutine is still holding
     * this conn alive. Prevents the classic async UAF where a
     * read-callback CLOSING path (peer FIN, close_notify, parse
     * error) frees strategy/session while a dispose coroutine is
     * suspended inside commit/send drain — reproducibly SEGVs with
     * conn->io / session pointer clobbered by ASCII debris from the
     * fresh alloc that reused the memory. Pinned by:
     *   - HTTP/2: http2_strategy.c:165 (per-stream dispatch)
     *   - HTTP/1: http_connection_dispatch_request (TLS pipelining)
     * The last handler's dispose re-enters destroy via destroy_pending;
     * by then handler_refcount is zero and the free proceeds. */
    if (conn->handler_refcount > 0) {
        conn->destroy_pending = true;
        return;
    }
#ifdef HAVE_OPENSSL
    /* Defer destroy while an FSM async send is in flight. libuv still
     * holds a pointer into our heap buffer for the pending uv_write;
     * detaching the completion callback now would orphan the only path
     * that frees that buffer. The send callback re-runs destroy after
     * the write completes (or is cancelled by the close path). */
    if (http_connection_tls_fsm_send_in_flight(conn)) {
        conn->destroy_pending = true;
        return;
    }
#endif

    /* Notify server of close so it can decrement active_connections and
     * maybe resume listeners. Per-request timing is already reported by
     * http_server_on_request_sample from the coroutine entry — no
     * timing carried on the connection itself. Safe with server == NULL. */
    http_server_on_connection_close(conn->server);

    /* Slot is unlinked from the arena's alive list inside
     * conn_arena_free at the end of destroy. No O(N) walk needed —
     * O(1) doubly-linked unlink via prev_conn/next_conn. */

    /* Detach the persistent read callback first. If we let
     * ZEND_ASYNC_IO_CLOSE dispose the event with our callback still in
     * the list, a late read-completion notify could fire the callback
     * with a dangling conn pointer.
     *
     * zend_async_callbacks_remove() calls our dispose hook directly and
     * efrees the struct, regardless of ref_count — do NOT also call
     * RELEASE on the same pointer, it would be use-after-free.
     *
     * conn->read_cb stores either the plaintext or the TLS-FSM
     * variant; both are wired via base.dispose, so the fallback path
     * (conn->io already gone) routes through the stored function
     * pointer rather than hard-coding the plaintext one. */
    if (conn->read_cb) {
        zend_async_event_callback_t *base_cb = &conn->read_cb->base;
        if (conn->io) {
            zend_async_callbacks_remove(&conn->io->event, base_cb);
        } else if (base_cb->dispose != NULL) {
            base_cb->dispose(base_cb, NULL);
        }
        conn->read_cb = NULL;
    }
#ifdef HAVE_OPENSSL
    /* TLS-FSM async-send callback (separate slot from read_cb). Same
     * detach rules; same dispose-pointer fallback. */
    if (conn->tls_fsm_send_cb) {
        zend_async_event_callback_t *send_cb_base =
            (zend_async_event_callback_t *)conn->tls_fsm_send_cb;
        if (conn->io) {
            zend_async_callbacks_remove(&conn->io->event, send_cb_base);
        } else if (send_cb_base->dispose != NULL) {
            send_cb_base->dispose(send_cb_base, NULL);
        }
        conn->tls_fsm_send_cb = NULL;
    }
#endif

    /* Per-conn write timer must die before io: callback would
     * otherwise touch a dangling conn pointer if it fires during
     * io close. */
    http_write_timer_dispose(conn);

    /* The IO handle closes the underlying socket asynchronously via uv_close
     * and disposes its own event when the close callback fires. We also
     * release the ref handed to us by ZEND_ASYNC_IO_CREATE — otherwise
     * close_cb's own DEL_REF only brings the count back to 1 and the
     * async_io_t (plus its callbacks vector) leaks on server teardown. */
    if (conn->io) {
        zend_async_io_t *io = conn->io;
        conn->io = NULL;
        ZEND_ASYNC_IO_CLOSE(io);
        io->event.dispose(&io->event);
    }

    if (conn->strategy) {
        if (conn->strategy->cleanup) {
            conn->strategy->cleanup(conn);
        }
        http_protocol_strategy_destroy(conn->strategy);
        conn->strategy = NULL;
    }

    if (conn->parser) {
        parser_pool_return(conn->parser);
        conn->parser = NULL;
    }

    if (conn->read_buffer) {
        efree(conn->read_buffer);
    }

#ifdef HAVE_OPENSSL
    /* TLS session lives only as long as its connection. Free *after*
     * the IO handle is torn down so any in-flight write can still
     * reach drain_ciphertext — but before efree(conn) obviously. */
    if (conn->tls) {
        tls_session_free(conn->tls);
        conn->tls = NULL;
    }
    /* Plaintext queue + flusher state. By the time destroy runs the
     * handler_refcount gate has guaranteed no producer is mid-push, so
     * the BIOs are unowned. drain_event might still carry a waker if a
     * producer was backpressured when destroy_pending fired; disposing
     * the event resolves the waker with conn->tls == NULL, which makes
     * await_tls_drain_event return false. */
    if (conn->tls_plaintext_bio) {
        BIO_free(conn->tls_plaintext_bio);
        conn->tls_plaintext_bio = NULL;
    }
    if (conn->tls_plaintext_bio_app) {
        BIO_free(conn->tls_plaintext_bio_app);
        conn->tls_plaintext_bio_app = NULL;
    }
    if (conn->tls_drain_event) {
        conn->tls_drain_event->base.dispose(&conn->tls_drain_event->base);
        conn->tls_drain_event = NULL;
    }
#endif

    /* Capture the owning C-state pointer before returning the slot
     * to the arena — `conn` is gone after the next call. The release
     * after that drops our ref; if the PHP wrapper already gave up
     * its ref and this was the last live conn, http_server_release
     * runs the finalizer (arena cleanup) and pefrees the C-state. */
    http_server_object *owning_server = conn->server;
    if (owning_server != NULL) {
        conn_arena_free(http_server_arena(owning_server), conn);
    } else {
        efree(conn);
    }
    http_server_release(owning_server);
}
/* }}} */

/* {{{ http_io_req_event_t - per-await event for a single IO req.
 *
 * Problem we are solving:
 *   io->event is shared by every read AND write on this handle: libuv's
 *   io_pipe_read_cb and io_pipe_write_cb both fire NOTIFY on the same
 *   event. If the handler coroutine awaits directly on io->event, any
 *   unrelated completion (e.g. a read chunk arriving while we wait for a
 *   write, or vice-versa) would falsely wake it. Worse, zend_async_waker
 *   _clean → waker_events_dtor calls event->stop(io->event) when the
 *   coroutine resumes — and libuv_io_event_stop kills uv_read_start.
 *   For HTTP/2 streaming this means: every handler send() teardown
 *   disarms the persistent read callback, so subsequent WINDOW_UPDATE
 *   frames never reach nghttp2 and the stream deadlocks.
 *
 * Solution — an in-memory event private to one await:
 *   - Our own zend_async_event_t with no libuv backing. event->stop /
 *     dispose are self-contained; they don't touch io->event.
 *   - We register a one-shot filter callback on io->event that watches
 *     for a NOTIFY whose result matches our req. On a match it NOTIFIES
 *     our private event (waking just the expected coroutine) and
 *     detaches itself.
 *   - info() returns a human-readable description so the scheduler's
 *     deadlock / debug printout names what the coroutine is waiting on.
 *
 * Lifecycle:
 *   - Created in async_io_req_await.
 *   - Ownership transferred to the waker (trans_event=true in
 *     zend_async_resume_when). The waker disposes it after the coroutine
 *     resumes.
 *   - Our dispose detaches the filter callback from io->event if it was
 *     not already detached (i.e. if the coroutine was cancelled / timed
 *     out before the req completed). */
typedef struct http_io_req_event_s http_io_req_event_t;

typedef struct {
    zend_async_event_callback_t  base;
    http_io_req_event_t         *owner;
} http_io_req_filter_cb_t;

/* Direction is purely informational (for the info() method). The filter
 * matches by req pointer, not by direction. http_io_req_op_t lives in
 * http_connection_internal.h since http_connection_tls.c also passes
 * direction to async_io_req_await. */

struct http_io_req_event_s {
    zend_async_event_t            base;
    zend_async_io_req_t          *expected_req;
    zend_async_io_t              *io;
    http_io_req_filter_cb_t      *filter_cb;  /* NULL once detached */
    http_io_req_op_t              op;
};

static bool http_io_req_event_add_callback(
    zend_async_event_t *event, zend_async_event_callback_t *callback)
{
    return zend_async_callbacks_push(event, callback);
}

static bool http_io_req_event_del_callback(
    zend_async_event_t *event, zend_async_event_callback_t *callback)
{
    return zend_async_callbacks_remove(event, callback);
}

static bool http_io_req_event_start(zend_async_event_t *event)
{
    /* Purely in-memory — nothing to arm. */
    (void)event;
    return true;
}

static bool http_io_req_event_stop(zend_async_event_t *event)
{
    /* Purely in-memory — nothing to disarm. Crucially: we do NOT touch
     * the underlying io->event here, unlike libuv_io_event_stop which
     * would call uv_read_stop and kill our persistent read. */
    (void)event;
    return true;
}

static void http_io_req_event_detach_filter(http_io_req_event_t *ev)
{
    if (ev->filter_cb == NULL || ev->io == NULL) {
        return;
    }
    zend_async_event_t *io_event = &ev->io->event;
    (void)zend_async_callbacks_remove(io_event, &ev->filter_cb->base);
    ev->filter_cb = NULL;
}

static bool http_io_req_event_dispose(zend_async_event_t *event)
{
    if (ZEND_ASYNC_EVENT_REFCOUNT(event) > 1) {
        ZEND_ASYNC_EVENT_DEL_REF(event);
        return true;
    }

    http_io_req_event_t *ev = (http_io_req_event_t *)event;
    ZEND_ASYNC_EVENT_SET_CLOSED(&ev->base);
    http_io_req_event_detach_filter(ev);

    zend_async_callbacks_free(event);
    efree(ev);
    return true;
}

static zend_string *http_io_req_event_info(zend_async_event_t *event)
{
    http_io_req_event_t *ev = (http_io_req_event_t *)event;
    const zend_async_io_t *io = ev->io;
    const char *op = ev->op == HTTP_IO_REQ_WRITE ? "write" : "read";

    if (io == NULL) {
        return zend_strpprintf(0, "IOReq(op=%s, req=%p)", op,
                               (void *)ev->expected_req);
    }
    if (io->type == ZEND_ASYNC_IO_TYPE_TCP || io->type == ZEND_ASYNC_IO_TYPE_UDP) {
        return zend_strpprintf(0, "IOReq(op=%s, socket=" ZEND_LONG_FMT ", req=%p)",
                               op, (zend_long)io->descriptor.socket,
                               (void *)ev->expected_req);
    }
    return zend_strpprintf(0, "IOReq(op=%s, fd=" ZEND_LONG_FMT ", req=%p)",
                           op, (zend_long)io->descriptor.fd,
                           (void *)ev->expected_req);
}

/* Filter callback installed on io->event. io->event is notified for every
 * completion (read or write) on this handle; we forward only the one whose
 * result matches our expected_req. Handle-level exceptions propagate
 * unconditionally so no waiter hangs on a closed handle. */
static void http_io_req_filter_cb_fn(zend_async_event_t *io_event,
                                     zend_async_event_callback_t *callback,
                                     void *result,
                                     zend_object *exception)
{
    http_io_req_filter_cb_t *cb = (http_io_req_filter_cb_t *)callback;
    http_io_req_event_t *ev = cb->owner;

    if (ev == NULL) {
        return;
    }
    if (exception == NULL && result != ev->expected_req) {
        return;
    }

    /* One-shot: detach before notifying so dispose (which may run
     * synchronously from the waker) doesn't try to remove us twice. */
    (void)zend_async_callbacks_remove(io_event, &cb->base);
    ev->filter_cb = NULL;

    ZEND_ASYNC_CALLBACKS_NOTIFY(&ev->base, result, exception);
}

static void http_io_req_filter_cb_dispose(
    zend_async_event_callback_t *callback, zend_async_event_t *event)
{
    (void)event;
    efree(callback);
}

static http_io_req_event_t *http_io_req_event_new(
    zend_async_io_t *io, zend_async_io_req_t *req, http_io_req_op_t op)
{
    http_io_req_event_t *ev = ecalloc(1, sizeof(*ev));
    ev->base.ref_count      = 1;
    ev->base.add_callback   = http_io_req_event_add_callback;
    ev->base.del_callback   = http_io_req_event_del_callback;
    ev->base.start          = http_io_req_event_start;
    ev->base.stop           = http_io_req_event_stop;
    ev->base.dispose        = http_io_req_event_dispose;
    ev->base.info           = http_io_req_event_info;
    ev->expected_req        = req;
    ev->io                  = io;
    ev->op                  = op;

    http_io_req_filter_cb_t *cb = ecalloc(1, sizeof(*cb));
    cb->base.callback = http_io_req_filter_cb_fn;
    cb->base.dispose  = http_io_req_filter_cb_dispose;
    cb->owner         = ev;
    ev->filter_cb     = cb;

    if (!io->event.add_callback(&io->event, &cb->base)) {
        efree(cb);
        ev->filter_cb = NULL;
        efree(ev);
        return NULL;
    }
    return ev;
}
/* }}} */

/* {{{ async_io_req_await - Wait for a ZEND_ASYNC_IO_READ/WRITE request to
 *                          complete, with optional timeout in ms.
 *
 * The request returned by IO_READ/IO_WRITE may already be completed when we
 * get it (sync fast path — e.g. Windows _read fallback or EOF). Otherwise we
 * create a private http_io_req_event_t (see above), have the coroutine
 * suspend on THAT (not on the shared io->event), and let our filter
 * callback forward only the matching req completion. This keeps other
 * operations on the same io handle — notably the persistent read
 * callback — unaffected by this coroutine's wake/sleep cycle.
 *
 * Returns: true if req is completed normally, false on error/timeout. The
 * caller is always responsible for calling req->dispose(req).
 */
/* Cross-TU: also called from http_connection_tls_socket_read in
 * http_connection_tls.c. Declared in http_connection_internal.h. */
bool async_io_req_await(zend_async_io_req_t *req, zend_async_io_t *io,
                        const uint32_t timeout_ms, http_io_req_op_t op,
                        http_log_state_t *log_state)
{
    if (req->completed) {
        return true;
    }

    zend_coroutine_t *coroutine = ZEND_ASYNC_CURRENT_COROUTINE;
    if (ZEND_ASYNC_WAKER_NEW(coroutine) == NULL) {
        return false;
    }

    /* Skip per-call timeout timer for writes. libuv surfaces peer
     * RST/EOF/EPIPE through io_pipe_write_cb so a write either
     * completes or returns an exception on req — it never hangs
     * forever. Slowloris-write (peer keeps conn open but ACKs
     * slowly) is bounded by the OS TCP retransmit timeout (~30 s);
     * a per-conn rearmed write timer addresses that gap in a
     * follow-up. Reads still get the timer: a silent client could
     * hold an idle connection without triggering libuv. */
    if (timeout_ms > 0 && op == HTTP_IO_REQ_READ) {
        zend_async_resume_when(
            coroutine,
            &ZEND_ASYNC_NEW_TIMER_EVENT((zend_ulong)timeout_ms, false)->base,
            true,
            zend_async_waker_callback_timeout,
            NULL
        );
    }

    http_io_req_event_t *wait_ev = http_io_req_event_new(io, req, op);
    if (UNEXPECTED(wait_ev == NULL)) {
        return false;
    }

    /* trans_event=true → the waker takes ownership and disposes the event
     * after the coroutine resumes; dispose detaches our filter from
     * io->event. */
    zend_async_resume_when(coroutine, &wait_ev->base, true,
                           zend_async_waker_callback_resolve, NULL);

    ZEND_ASYNC_SUSPEND();
    zend_async_waker_clean(coroutine);

    if (EG(exception) != NULL) {
        /* Timeout exception — clear it if we can still use the req
         * (sometimes libuv completes just before the timer fires). */
        if (req->completed && instanceof_function(
                EG(exception)->ce,
                ZEND_ASYNC_GET_EXCEPTION_CE(ZEND_ASYNC_EXCEPTION_TIMEOUT))) {
            http_logf_debug(log_state,
                            "connection.io.timeout_race ce=%s",
                            ZSTR_VAL(EG(exception)->ce->name));
            zend_clear_exception();
            return true;
        }
        /* I/O exception from the reactor (io_pipe_write_cb etc.) is
         * delivered BOTH on req->exception and, via the waker resolve
         * path, into EG. The caller inspects req->exception as the
         * canonical signal; the EG copy would otherwise surface as an
         * uncaught top-level exception on the next handler suspend
         * — reproducible under concurrent h2+TLS load via h2load.
         *
         * Narrow the clear strictly to InputOutputException. Any other
         * exception here (cancellation from cb_on_stream_close on peer
         * RST, user-thrown via ZEND_ASYNC_CANCEL, deadlock signal,
         * etc.) is an external control-flow event the handler must
         * see — do NOT silence those. */
        if (instanceof_function(EG(exception)->ce,
                ZEND_ASYNC_GET_EXCEPTION_CE(ZEND_ASYNC_EXCEPTION_INPUT_OUTPUT))) {
            http_logf_debug(log_state,
                            "connection.io.absorbed ce=%s op=%d",
                            ZSTR_VAL(EG(exception)->ce->name), (int)op);
            zend_clear_exception();
        }
        return false;
    }

    return req->completed;
}
/* }}} */

/* {{{ Per-connection write deadline timer.
 *
 * One timer struct + one callback per conn (lazy-created). Each send
 * rearms it; successful completion stops it. Fire path force-closes
 * io which propagates an exception to the suspended writer's req.
 */
typedef struct {
    zend_async_event_callback_t  base;
    http_connection_t           *conn;
} http_write_timer_cb_t;

static void http_write_timer_cb_dispose(zend_async_event_callback_t *cb,
                                        zend_async_event_t *event)
{
    (void)event;
    efree(cb);
}

static void http_write_timer_cb_fn(zend_async_event_t *event,
                                   zend_async_event_callback_t *callback,
                                   void *result,
                                   zend_object *exception)
{
    (void)event; (void)result; (void)exception;
    http_write_timer_cb_t *cb = (http_write_timer_cb_t *)callback;
    if (cb->conn == NULL) {
        return;
    }
    cb->conn->write_timed_out = true;
    /* Force-fail in-flight write — closing io marks any pending req
     * with an error which wakes the suspended writer in
     * async_io_req_await. */
    if (cb->conn->io != NULL) {
        ZEND_ASYNC_IO_CLOSE(cb->conn->io);
    }
}

static bool http_write_timer_arm(http_connection_t *conn, const uint32_t ms)
{
    if (conn->write_timer == NULL) {
        zend_async_timer_event_t *t =
            ZEND_ASYNC_NEW_TIMER_EVENT((zend_ulong)ms, false);
        if (UNEXPECTED(t == NULL)) {
            return false;
        }
        /* Mark MULTISHOT — required by ZEND_ASYNC_TIMER_REARM (libuv_timer_rearm
         * silently rejects non-multishot timers; the next arm would no-op,
         * leaving the timer stopped with rc=0, and the matching
         * http_write_timer_stop would call .stop() on a rc=0 event which
         * falls through EVENT_STOP_PROLOGUE and STEALS a count from the
         * shared active_event_count global → eventual false-positive
         * Async\DeadlockError. Surfaced under h2spec at test #73 as the
         * h2spec_server.php deadlock investigated 2026-04-28. */
        ZEND_ASYNC_TIMER_SET_MULTISHOT(t);
        http_write_timer_cb_t *cb = (http_write_timer_cb_t *)
            ZEND_ASYNC_EVENT_CALLBACK_EX(http_write_timer_cb_fn, sizeof(*cb));
        if (UNEXPECTED(cb == NULL)) {
            t->base.dispose(&t->base);
            return false;
        }
        cb->base.dispose = http_write_timer_cb_dispose;
        cb->conn = conn;
        if (!t->base.add_callback(&t->base, &cb->base)) {
            efree(cb);
            t->base.dispose(&t->base);
            return false;
        }
        conn->write_timer    = &t->base;
        conn->write_timer_cb = &cb->base;
        if (!t->base.start(&t->base)) {
            return false;
        }
        return true;
    }
    /* Reuse: rearm to (now + ms). */
    zend_async_timer_event_t *t = (zend_async_timer_event_t *)conn->write_timer;
    ZEND_ASYNC_TIMER_REARM(t, (zend_ulong)ms, 0);
    return true;
}

static void http_write_timer_stop(http_connection_t *conn)
{
    if (conn->write_timer == NULL) {
        return;
    }
    /* Skip if already stopped — calling .stop() on a timer with rc=0
     * falls through EVENT_STOP_PROLOGUE and the body's unconditional
     * DECREASE_EVENT_COUNT steals a count from another event. Belt-
     * and-braces guard alongside the MULTISHOT flag in arm(). */
    if (conn->write_timer->loop_ref_count == 0) {
        return;
    }
    conn->write_timer->stop(conn->write_timer);
}

static void http_write_timer_dispose(http_connection_t *conn)
{
    if (conn->write_timer_cb != NULL) {
        http_write_timer_cb_t *cb = (http_write_timer_cb_t *)conn->write_timer_cb;
        cb->conn = NULL;  /* defensive: in-flight callback won't touch conn */
        if (conn->write_timer != NULL) {
            zend_async_callbacks_remove(conn->write_timer, conn->write_timer_cb);
        } else if (conn->write_timer_cb->dispose != NULL) {
            conn->write_timer_cb->dispose(conn->write_timer_cb, NULL);
        }
        conn->write_timer_cb = NULL;
    }
    if (conn->write_timer != NULL) {
        conn->write_timer->dispose(conn->write_timer);
        conn->write_timer = NULL;
    }
}
/* }}} */

/* {{{ http_connection_handle_read_completion
 *
 * Process whatever just landed in conn->read_buffer (via either a
 * sync-complete ZEND_ASYNC_IO_READ or the async read callback) and
 * feed it into the protocol parser.
 *
 * Return value:
 *   true  → caller should re-arm another read (more bytes needed —
 *           either still parsing headers, or streaming body chunks
 *           into an already-dispatched request).
 *   false → the connection has been handed off or destroyed; caller
 *           must not touch it. Either:
 *             - parse error → destroyed
 *             - parser reached on_message_complete → the handler
 *               coroutine owns conn now, it re-arms for keep-alive
 *               itself.
 */
static bool http_connection_handle_read_completion(http_connection_t *conn,
                                                   bool *should_destroy_out)
{
    *should_destroy_out = false;

    if (UNEXPECTED(!conn->protocol_detected) && !detect_and_assign_protocol(conn)) {
        return true;  /* Need more data for detection — caller re-arms */
    }

    if (!conn->strategy || !conn->strategy->feed) {
        *should_destroy_out = true;
        return false;
    }

    /* Feed the buffered chunk into the protocol parser. feed() may
     * synchronously fire on_request_ready from inside llhttp's
     * on_headers_complete — at that point a handler coroutine is
     * enqueued. llhttp pauses at on_message_complete so any pipelined
     * bytes that follow stay in our buffer for the next request. */
    size_t consumed = 0;
    const int result = conn->strategy->feed(
        conn->strategy, conn, conn->read_buffer, conn->read_buffer_len, &consumed);

    if (result < 0) {
        /* RFC 9112 §5.4 / 9110 §15.5: emit a 4xx before close so the
         * peer learns *why* its request was rejected (vs an opaque
         * TCP RST). Two sub-paths:
         *
         *   - Handler already dispatched (mid-stream body limit hit
         *     after on_headers_complete): cancel the handler coroutine
         *     via the helper. It decides Case A / Case B based on
         *     whether user PHP has begun running, and emits or
         *     suppresses our 4xx accordingly. Handler dispose owns
         *     destroy from there — we return should_destroy=false.
         *
         *   - No handler dispatched (URI / header limit hit before
         *     on_headers_complete, or Content-Length pre-checked in
         *     on_headers_complete): emit the 4xx ourselves and signal
         *     destroy via should_destroy=true. Caller must disarm the
         *     multishot read + dispose its req before destroy frees
         *     rcb out from under it.
         */
        if (conn->current_request != NULL && conn->current_request->coroutine != NULL) {
            http_connection_cancel_handler_for_parse_error(conn);
            *should_destroy_out = false;
            return false;
        }
        if (conn->parser != NULL) {
            (void)http_connection_emit_parse_error(conn, conn->parser);
        }
        *should_destroy_out = true;
        return false;
    }

    /* Shift any unconsumed bytes (pipelined tail) down to the buffer
     * start so the next feed sees them at offset 0. */
    if (consumed < conn->read_buffer_len) {
        const size_t remaining = conn->read_buffer_len - consumed;
        memmove(conn->read_buffer, conn->read_buffer + consumed, remaining);
        conn->read_buffer_len = remaining;
    } else {
        conn->read_buffer_len = 0;
    }

    /* If the parser has reached on_message_complete, the body is
     * fully materialised. The handler coroutine (already queued or
     * running) will take over. Mark request_in_flight so the multishot
     * read callback buffers further bytes without parsing — handler
     * dispose drains the pipelined tail before clearing the flag. */
    if (conn->parser && http_parser_is_complete(conn->parser)) {
        conn->request_in_flight = true;
        return false;
    }

    /* Still need more data — either headers not done, or body chunks
     * still streaming. Caller re-arms. */
    return true;
}
/* }}} */

/* {{{ http_connection_read_callback_dispose
 *
 * Default dispose for the persistent read callback. Just efrees — the
 * connection owns conn->read_cb explicitly and has already nulled the
 * back-reference by the time this fires (either from the event's own
 * teardown after ZEND_ASYNC_IO_CLOSE or from our own RELEASE call).
 */
static void http_connection_read_callback_dispose(
    zend_async_event_callback_t *callback,
    zend_async_event_t *event)
{
    (void)event;
    efree(callback);
}
/* }}} */

/* {{{ http_connection_read_callback_fn
 *
 * Event-loop callback fired from io->event.notify when a read or any
 * other io-bound notification lands. We filter by active_req — if the
 * notify isn't for our outstanding read (e.g. a write completion that
 * fired during dispatch), we ignore it.
 */
static void http_connection_read_callback_fn(
    zend_async_event_t *event,
    zend_async_event_callback_t *callback,
    void *result,
    zend_object *exception)
{
    (void)event;
    http_connection_read_cb_t *rcb = (http_connection_read_cb_t *)callback;
    http_connection_t *conn = rcb->conn;

    /* This callback is a persistent listener on io->event, which is
     * shared by ALL IO operations (both read and write) on this handle.
     * When a write completes (e.g. from http_connection_send in the
     * handler coroutine), ZEND_ASYNC_CALLBACKS_NOTIFY fires on the
     * same io->event, so we receive that notification too.
     *
     * We distinguish "our" read completion from other operations by
     * comparing the result pointer against active_req — each
     * ZEND_ASYNC_IO_READ/WRITE creates a unique io_req_t.
     *
     * active_req is set by http_connection_read and cleared here after processing.
     * Between http_connection_read calls it stays NULL, which also filters out
     * any write completions that arrive while no read is outstanding. */
    if (UNEXPECTED(conn == NULL || rcb->active_req == NULL)) {
        return;
    }

    if (UNEXPECTED(result != rcb->active_req)) {
        return;
    }

    zend_async_io_req_t *req = rcb->active_req;

    /* Multishot: req stays alive until a terminal event (EOF/error), each
     * chunk fires this callback with completed=false. One-shot / sync paths
     * arrive with completed=true and must dispose the req. */
    const bool terminal = req->completed;
    const bool err = (exception != NULL) || (req->exception != NULL);
    const ssize_t bytes_read = req->transferred;

    if (UNEXPECTED(req->exception != NULL)) {
        OBJ_RELEASE(req->exception);
        req->exception = NULL;
    }

    if (UNEXPECTED(terminal)) {
        rcb->active_req = NULL;
        req->dispose(req);
    }

    if (UNEXPECTED(err || bytes_read <= 0)) {
        if (!terminal) {
            rcb->active_req = NULL;
            conn->io->event.stop(&conn->io->event);
            req->dispose(req);
        }
        /* Defer destroy if a handler is in flight or the read_buffer holds
         * a pipelined tail — flagging keep_alive=false makes handler dispose
         * tear the conn down once it (and any pipelined chain) has finished
         * responding. Without this, an EOF from a peer that sent its last
         * request and shut down the write half kills mid-flight responses. */
        if (conn->request_in_flight || conn->read_buffer_len > 0) {
            conn->keep_alive = false;
            return;
        }
        http_connection_destroy(conn);
        return;
    }

    conn->read_buffer_len += bytes_read;

    /* Re-entrancy guard: a handler coroutine is currently in flight (possibly
     * suspended at an await), so the parser must not run — feeding new bytes
     * could on_message_complete and dispatch a *second* handler on the same
     * conn while the first one's response slot is still live. Just buffer the
     * tail; handler dispose will pull it out via handle_read_completion. */
    if (!terminal && conn->request_in_flight) {
        return;
    }

    bool should_destroy = false;
    if (!http_connection_handle_read_completion(conn, &should_destroy)) {
        if (should_destroy) {
            http_connection_destroy(conn);
        }
        /* Multishot reader stays armed on the same req; alloc_cb will
         * point libuv at the correct offset on the next chunk. */
        return;
    }

    /* Still need more data. In multishot the reader is already armed on the
     * same req — just wait for the next chunk. Only re-arm for the rare
     * terminal-but-need-more-data case (sync fast path on a non-EOF
     * completion is not expected, but defensive). */
    if (terminal) {
        http_connection_read(conn);
    }
}
/* }}} */

/* Per-chunk allocator wired into conn->io->alloc_cb. Hands the reactor
 * the next free slice of read_buffer so multishot keeps writing at the
 * correct offset across requests and pipelined tails. */
static void http_connection_alloc_cb(zend_async_io_t *io, size_t suggested, zend_async_buf_t *out)
{
    (void)suggested;
    http_connection_t *conn = (http_connection_t *) io->user_data;
    if (UNEXPECTED(conn == NULL || conn->read_buffer_len >= conn->read_buffer_size)) {
        out->base = NULL;
        out->len  = 0;
        return;
    }
    out->base = conn->read_buffer + conn->read_buffer_len;
    out->len  = conn->read_buffer_size - conn->read_buffer_len;
}

/* {{{ http_connection_read */
bool http_connection_read(http_connection_t *conn)
{
    if (conn->read_buffer_len >= conn->read_buffer_size) {
        http_connection_destroy(conn);
        return false;
    }

    zend_async_io_req_t *req = ZEND_ASYNC_IO_READ(
        conn->io,
        conn->read_buffer + conn->read_buffer_len,
        conn->read_buffer_size - conn->read_buffer_len);
    if (req == NULL) {
        http_connection_destroy(conn);
        return false;
    }

    /* Sync-complete fast path — no need to arm a callback. */
    if (req->completed) {
        const bool err = (req->exception != NULL);
        const ssize_t bytes_read = req->transferred;
        if (req->exception != NULL) {
            OBJ_RELEASE(req->exception);
            req->exception = NULL;
        }
        req->dispose(req);

        if (err || bytes_read <= 0) {
            http_connection_destroy(conn);
            return false;
        }

        conn->read_buffer_len += bytes_read;

        bool should_destroy = false;
        if (!http_connection_handle_read_completion(conn, &should_destroy)) {
            if (should_destroy) {
                http_connection_destroy(conn);
            }
            return false;
        }

        /* Headers still incomplete — re-arm (tail-recursion is fine,
         * sync completions are finite). */
        return http_connection_read(conn);
    }

    /* Async path: allocate the persistent read callback on first use
     * and attach it to io->event. Subsequent arms only update
     * active_req.
     *
     * NOTE: the `size` argument to ZEND_ASYNC_EVENT_CALLBACK_EX is the
     * TOTAL allocation size (the API asserts it >= sizeof base) — not
     * extra bytes beyond the base. Passing the wrong value here is a
     * heap-buffer overflow waiting to happen. */
    if (conn->read_cb == NULL) {
        http_connection_read_cb_t *rcb = (http_connection_read_cb_t *)
            ZEND_ASYNC_EVENT_CALLBACK_EX(http_connection_read_callback_fn,
                                          sizeof(http_connection_read_cb_t));
        if (rcb == NULL) {
            req->dispose(req);
            http_connection_destroy(conn);
            return false;
        }
        rcb->base.dispose = http_connection_read_callback_dispose;
        rcb->conn = conn;
        rcb->active_req = NULL;

        if (!conn->io->event.add_callback(&conn->io->event, &rcb->base)) {
            http_connection_read_callback_dispose(&rcb->base, NULL);
            req->dispose(req);
            http_connection_destroy(conn);
            return false;
        }
        conn->read_cb = rcb;
    }

    conn->read_cb->active_req = req;
    return true;
}
/* }}} */

/* {{{ http_connection_send_raw
 *
 * Synchronous socket-level send (from a coroutine — uses async_io_req_await).
 * Splits the buffer into multiple IO_WRITE calls as the reactor accepts
 * whatever size it can; loops until @p len bytes are on the wire.
 *
 * Factored out of the public http_connection_send() so the TLS path can
 * produce ciphertext via tls_write_plaintext + tls_drain_ciphertext and
 * feed that ciphertext straight into the socket without reintroducing
 * the TLS branch on the inner loop. Internal — direct callers are the
 * plaintext path and the TLS encrypt path.
 */
/* Cross-TU: also called from http_connection_tls_drain_ring_to_socket. */
bool http_connection_send_raw(http_connection_t *conn,
                                     const char *data, const size_t len)
{
    if (len == 0) {
        return true;
    }
    if (UNEXPECTED(conn->write_timed_out)) {
        return false;
    }

    const uint32_t write_timeout_ms = conn->write_timeout_ms;

    /* Arm the per-conn write deadline once for the whole send. The
     * timer fires only if the entire send (across multiple await
     * iterations) takes longer than write_timeout_ms. */
    if (write_timeout_ms > 0 && !http_write_timer_arm(conn, write_timeout_ms)) {
        return false;
    }

    /* Single-shot submit. uv_write is all-or-error from the caller's
     * perspective: io_pipe_write_cb sets transferred = max_size on
     * status==0 or transferred = -1 with an exception on failure. The
     * pre-existing while-loop iterating on partial-write was dead code:
     * partial absorption is handled internally by libuv (uv_try_write
     * + EPOLLOUT-driven drain). */
    bool ok_total = false;
    zend_async_io_req_t *req = ZEND_ASYNC_IO_WRITE(conn->io, data, len);
    if (req != NULL) {
        const bool ok = async_io_req_await(req, conn->io, write_timeout_ms,
                                           HTTP_IO_REQ_WRITE,
                                           conn->log_state);
        const bool had_exc = (req->exception != NULL);
        if (had_exc) {
            OBJ_RELEASE(req->exception);
            req->exception = NULL;
        }
        const ssize_t transferred = req->transferred;
        req->dispose(req);
        ok_total = ok && !had_exc && transferred == (ssize_t)len;
    }

    if (write_timeout_ms > 0) {
        http_write_timer_stop(conn);
    }
    if (UNEXPECTED(conn->write_timed_out)) {
        return false;
    }
    return ok_total;
}
/* }}} */

/* {{{ http_connection_send_str_owned
 *
 * Fire-and-forget plaintext send: transfer ownership of @p body to the
 * reactor. Returns immediately after submit; the buffer is released
 * when libuv reports completion (success or error), without parking
 * the calling coroutine. Hot path for the HTTP/1 dispose flow — avoids
 * the suspend/resume cycle around uv_try_write that absorbs the whole
 * payload inline in the steady state.
 *
 * On submit failure the body is released here. On any kernel-level
 * write error, the io is closed by libuv and the next read attempt on
 * this conn surfaces the failure to the read FSM, which tears the
 * connection down — the just-finished handler does not need to know.
 */
static void http1_send_release_zstr_cb(void *data, zend_async_io_t *io)
{
    (void)io;
    zend_string *str = (zend_string *)((char *)data - offsetof(zend_string, val));
    zend_string_release(str);
}

bool http_connection_send_str_owned(http_connection_t *conn, zend_string *body)
{
    if (body == NULL) {
        return true;
    }
    if (ZSTR_LEN(body) == 0) {
        zend_string_release(body);
        return true;
    }
    if (UNEXPECTED(conn->write_timed_out)) {
        zend_string_release(body);
        return false;
    }

    zend_async_io_req_t *req = ZEND_ASYNC_IO_WRITE_EX(conn->io,
                                                     ZSTR_VAL(body),
                                                     ZSTR_LEN(body),
                                                     http1_send_release_zstr_cb);
    if (UNEXPECTED(req == NULL)) {
        /* libuv_io_req_dispose ran free_cb on the partially-built req
         * and the body was released there. Caller must not touch body. */
        return false;
    }
    /* Caller does not await, does not dispose. The completion callback
     * (io_pipe_write_cb) frees the body and disposes the req. */
    return true;
}
/* }}} */

/* {{{ http_connection_send_strv_owned
 *
 * Vectored fire-and-forget plaintext send. Each slot of @p bufs is an
 * OWNED zend_string reference; ZEND_ASYNC_IO_WRITEV consumes one ref per
 * slot on completion. Used for the HTTP/1 dispose hot path where
 * headers and body live in two separate zend_strings — saves one
 * emalloc + memcpy that http_response_format would otherwise spend
 * concatenating them. TLS path stays on the single-buffer
 * http_connection_send (encryption ring needs a contiguous payload).
 */
bool http_connection_send_strv_owned(http_connection_t *conn,
                                     zend_string * const *bufs, unsigned nbufs)
{
    if (UNEXPECTED(nbufs == 0)) {
        return true;
    }
    if (UNEXPECTED(conn->write_timed_out)) {
        for (unsigned i = 0; i < nbufs; i++) {
            zend_string_release(bufs[i]);
        }
        return false;
    }

    zend_async_io_req_t *req = ZEND_ASYNC_IO_WRITEV(conn->io, bufs, nbufs);
    if (UNEXPECTED(req == NULL)) {
        /* Reactor already released every slot on submit failure. */
        return false;
    }
    return true;
}
/* }}} */


/* {{{ http_connection_send
 *
 * Public send: plaintext in, plaintext or ciphertext out depending on
 * whether this connection has a TLS session attached. Callers above
 * the TLS layer (handler coroutine, error responder) stay oblivious
 * to encryption.
 */
bool http_connection_send(http_connection_t *conn, const char *data, const size_t len)
{
    if (!data || len == 0) {
        return true;
    }

#ifdef HAVE_OPENSSL
    if (conn->tls != NULL) {
        return tls_push_and_maybe_flush(conn, data, len);
    }
#endif

    return http_connection_send_raw(conn, data, len);
}
/* }}} */

/* {{{ http_connection_cancel_handler_for_parse_error
 *
 * Called from plaintext and TLS feed-error paths when on_headers_complete
 * already dispatched a handler (req->coroutine != NULL) before the
 * parser hit its limit. Cancels the handler with an HttpException
 * carrying the precise HTTP status — dispose later reads code+message
 * from the exception and builds the response from them
 * Telemetry is bumped here so even Case B (handler already running)
 * shows up in parse_errors_*_total.
 *
 * Two sub-cases naturally handled by the same code:
 *   - Handler not yet started: the cancellation exception is delivered
 *     to dispose, which sees an empty (uncommitted) response, builds
 *     a precise 4xx from the HttpException's code/message, sends it,
 *     destroys conn. Peer gets the right status.
 *   - Handler partway through user PHP: the exception is thrown into
 *     user code at the next suspend point. User code may catch it or
 *     let it propagate. Dispose still gets a chance to send the
 *     response — either user's own (if they committed before/during)
 *     or the fallback HttpException-derived 4xx.
 *
 * Connection teardown stays asynchronous via dispose.
 */
/* Cross-TU: also called from http_connection_tls.c parse-error path. */
void http_connection_cancel_handler_for_parse_error(http_connection_t *conn)
{
    if (conn->current_request == NULL || conn->current_request->coroutine == NULL) {
        return;
    }
    zend_coroutine_t *const h = conn->current_request->coroutine;
    conn->current_request->coroutine = NULL;

    const http_parse_error_t err = conn->parser ? conn->parser->parse_error
                                                : HTTP_PARSE_ERR_MALFORMED;
    const int   status = http_parse_error_to_status(err);
    const char *reason = http_parse_error_reason(err);

    /* Bump telemetry up front. The dispose-side response may or may
     * not actually go on the wire (socket may be dead by then), but
     * the parser-error event itself is what we want counted. */
    http_server_on_parse_error(conn->server, status);
    conn->keep_alive = false;

    /* Build HttpException(message=reason, code=status) directly
     * without EG(exception) side effects. Dispose reads both back
     * via zend_read_property to construct the response. */
    zval exception_zv, message_zv, code_zv;
    object_init_ex(&exception_zv, http_exception_ce);
    zend_object *const exc = Z_OBJ(exception_zv);

    ZVAL_STRING(&message_zv, reason);
    zend_update_property_ex(http_exception_ce, exc,
                            ZSTR_KNOWN(ZEND_STR_MESSAGE), &message_zv);
    zval_ptr_dtor(&message_zv);

    ZVAL_LONG(&code_zv, status);
    zend_update_property_ex(http_exception_ce, exc,
                            ZSTR_KNOWN(ZEND_STR_CODE), &code_zv);

    ZEND_ASYNC_CANCEL(h, exc, true);
}
/* }}} */

/* {{{ http_connection_emit_parse_error
 *
 * Build and send the RFC-compliant 4xx response for a parser failure.
 * Marks keep-alive=false and
 * bumps the parse-error telemetry counter unconditionally so DoS-
 * shaped traffic stays observable even when the socket is already
 * dead.
 *
 * Two call contexts:
 *   1. TLS coroutine — http_connection_send is safe; we encrypt and
 *      drain through the BIO pair, then the caller proceeds to
 *      graceful_close so the peer also sees close_notify.
 *   2. Plaintext read callback — runs outside any coroutine, so a
 *      suspending send would NPE on ZEND_ASYNC_CURRENT_COROUTINE. We
 *      fall back to a single sync-complete write (~80 bytes nearly
 *      always fit in the kernel send buffer); if the kernel can't
 *      take it all in one go we drop the rest. The peer then either
 *      gets the full response or no response — never a half-formed
 *      one — and the connection goes down right after.
 *
 * Best-effort either way. Returns true if the full response was
 * accepted by the transport.
 */
bool http_connection_emit_parse_error(http_connection_t *conn, http1_parser_t *parser)
{
    const http_parse_error_t err = parser ? parser->parse_error : HTTP_PARSE_ERR_MALFORMED;
    const int   status = http_parse_error_to_status(err);
    const char *reason = http_parse_error_reason(err);

    /* 256 B is plenty for the fixed status line + 4 headers + the
     * short reason body. The reason text doubles as the body
     * (matches envoy/nginx minimalism).
     *
     * Retry-After is emitted only on 503 (overload or OOM): clients and
     * load balancers honour it for back-off (AWS Builders' Library
     * "Using load shedding to avoid overload"). Other 4xx are permanent
     * for the given request, so retry would be pointless. */
    char response[256];
    const size_t reason_len = strlen(reason);
    const char *const extra_hdr = (status == 503) ? "Retry-After: 1\r\n" : "";
    const int n = snprintf(response, sizeof(response),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: text/plain; charset=utf-8\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "%s"
        "\r\n"
        "%s\r\n",
        status, reason, reason_len + 2, extra_hdr, reason);

    conn->keep_alive = false;
    http_server_on_parse_error(conn->server, status);

    if (n <= 0 || (size_t)n >= sizeof(response)) {
        return false;  /* truncation guard — unreachable for current reasons */
    }

    /* Coroutine context — full async send is safe. The scheduler-
     * context check is the load-bearing one: in a libuv callback
     * ZEND_ASYNC_CURRENT_COROUTINE is the *scheduler* coroutine
     * (non-NULL) but suspending from there throws "cannot be stopped
     * from the Scheduler context". */
    if (!ZEND_ASYNC_IS_SCHEDULER_CONTEXT && ZEND_ASYNC_CURRENT_COROUTINE != NULL) {
        return http_connection_send(conn, response, (size_t)n);
    }

#ifdef HAVE_OPENSSL
    /* TLS read FSM (no coroutine): SSL_write the response into the
     * cipher BIO and kick the FSM async-send path. Single SSL_write
     * always succeeds for a sub-256-byte response; the connection
     * transitions to CLOSING and destroy waits on the in-flight FSM
     * send so close_notify lands too. */
    if (conn->tls != NULL) {
        return http_connection_tls_fsm_send_plaintext_atomic(
            conn, response, (size_t)n);
    }
#endif

    /* Plaintext read-callback context — write directly to the
     * underlying socket in non-blocking mode. We bypass libuv's queue
     * here on purpose: ZEND_ASYNC_IO_WRITE wants a coroutine to await
     * the completion, and even disposing an unawaited req can race
     * with the close that immediately follows. The response is small
     * (~80 B), the socket is fresh, and the kernel send buffer is
     * effectively always able to absorb a single small write. */
    if (conn->io == NULL) {
        return false;
    }
    const php_socket_t fd = (php_socket_t)conn->io->descriptor.socket;
    if (fd == (php_socket_t)-1) {
        return false;
    }
    const ssize_t sent = send(fd, response, (size_t)n, MSG_NOSIGNAL);
    return sent == (ssize_t)n;
}
/* }}} */

/* {{{ http_connection_send_error */
bool http_connection_send_error(http_connection_t *conn, const int status_code, const char *message)
{
    char response[512];
    const int len = snprintf(response, sizeof(response),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n"
        "%s",
        status_code, message, strlen(message), message);

    return http_connection_send(conn, response, (size_t)len);
}
/* }}} */

/* {{{ http_connection_dispatch_request
 *
 * Called from on_request_ready (inside the read callback). Prepares
 * request + response PHP objects and spawns a handler coroutine that
 * will call the user's PHP handler. All cleanup (response sending,
 * keep-alive re-arm, object destruction) happens in the coroutine's
 * extended_dispose handler — http_handler_coroutine_dispose.
 */
static void http_connection_dispatch_request(http_connection_t *conn, http_request_t *req)
{
    /* Stash the live request pointer for cross-cutting paths (cancel,
     * parse-error). Per-request zvals live on the coroutine ctx, not
     * on conn — see http1_request_ctx_t. Cleared in dispose. */
    conn->current_request = req;

    /* Per-coroutine context. Carries request/response zvals so a
     * pipelined request N+1 dispatched while N's dispose is still
     * draining cannot clobber N's PHP objects. ecalloc zeroes both
     * zvals to IS_UNDEF. */
    http1_request_ctx_t *ctx = ecalloc(1, sizeof(*ctx));
    ctx->conn    = conn;
    ctx->request = req;

    zval *req_obj = http_request_create_from_parsed(req);
    ZVAL_COPY_VALUE(&ctx->request_zv, req_obj);
    efree(req_obj);

    /* Create HttpResponse PHP object */
    object_init_ex(&ctx->response_zv, http_response_ce);
    http_response_set_protocol_version(Z_OBJ(ctx->response_zv), conn->http_version);

    /* Install the HTTP/1 streaming vtable. send() on the response
     * activates chunked framing on first call; handlers that stick to
     * the buffered API (setBody / end) never touch stream_ops and the
     * regular http_response_format path runs in dispose. Opaque is
     * the per-coroutine ctx — h1_stream_ops needs response_zv +
     * h1_stream_headers_sent + a conn back-pointer, all on ctx. */
    extern const http_response_stream_ops_t h1_stream_ops;
    http_response_install_stream_ops(Z_OBJ(ctx->response_zv),
                                     &h1_stream_ops, ctx);

    conn->state = CONN_STATE_PROCESSING;

    zend_coroutine_t *coroutine = ZEND_ASYNC_NEW_COROUTINE(conn->scope);
    if (coroutine == NULL) {
        zval_ptr_dtor(&ctx->request_zv);
        zval_ptr_dtor(&ctx->response_zv);
        efree(ctx);
        http_connection_destroy(conn);
        return;
    }

    coroutine->internal_entry = http_handler_coroutine_entry;
    coroutine->extended_data = ctx;
    coroutine->extended_dispose = http_handler_coroutine_dispose;

    /* Remember the in-flight handler ON THE REQUEST (not the conn) so
     * a parse-error path (mid-stream body limit hit) can cancel it
     * via ZEND_ASYNC_CANCEL. Cleared at the very start of dispose —
     * past that point the coroutine is teardown-bound and
     * re-cancelling is a no-op. Per-request because one TCP carries
     * many requests; HTTP/2 stream multiplex inherits this naturally. */
    req->coroutine = coroutine;

    /* Bracket the handler lifetime on the server's in-flight counter.
     * Paired with http_server_on_request_dispose in the dispose fn —
     * drives admission-reject (see http_server_should_shed_request). */
    http_server_on_request_dispatch(conn->counters);

    /* Pin conn for the duration of the handler. Mirrors the HTTP/2
     * pattern at src/http2/http2_strategy.c:165. Without this pin a
     * read-callback CLOSING transition (peer FIN, close_notify, parse
     * error) can free conn while this handler's dispose is mid-send,
     * yielding a SIGSEGV when dispose touches conn->io after the
     * memory has been reused (the 065-tls-pipelining flake).
     * Decrement is at the tail of http_handler_coroutine_dispose. */
    conn->handler_refcount++;

    ZEND_ASYNC_ENQUEUE_COROUTINE(coroutine);
}
/* }}} */

/* {{{ http_handler_coroutine_entry
 *
 * Coroutine body: calls the user's PHP handler with (request, response).
 * Nothing else — all cleanup is in http_handler_coroutine_dispose.
 */
static void http_handler_coroutine_entry(void)
{
    const zend_coroutine_t *coroutine = ZEND_ASYNC_CURRENT_COROUTINE;
    http1_request_ctx_t *ctx = (http1_request_ctx_t *)coroutine->extended_data;
    http_connection_t *conn = ctx->conn;

    http_request_t *req = ctx->request;
    const bool stamps = http_server_sample_stamps_enabled(conn->view);
    if (req && stamps) {
        req->start_ns = zend_hrtime();
    }

    zval params[2], retval;
    ZVAL_COPY_VALUE(&params[0], &ctx->request_zv);
    ZVAL_COPY_VALUE(&params[1], &ctx->response_zv);
    ZVAL_UNDEF(&retval);

    /* Direct invoke via the cached fcall_info_cache — populated once
     * when addHttpHandler() ran Z_PARAM_FUNC. Skips the per-request
     * zend_is_callable_ex / zend_get_executed_filename_ex pair that
     * call_user_function (NULL fci_cache) would otherwise repeat for
     * every request on the hot path. */
    zend_fcall_info fci = {
        .size           = sizeof(zend_fcall_info),
        .function_name  = conn->handler->fci.function_name,
        .retval         = &retval,
        .params         = params,
        .object         = NULL,
        .param_count    = 2,
        .named_params   = NULL,
    };
    zend_call_function(&fci, &conn->handler->fci_cache);

    /* Stamp end_ns and fire the backpressure sample immediately after
     * the handler returned — BEFORE zval_ptr_dtor(retval) so destructor
     * time on the return value doesn't get counted as service time.
     * One sample per request: sojourn feeds CoDel, service feeds
     * telemetry. Fires even on handler exception (EG(exception) set) —
     * the measurement is still meaningful, work was done.
     * Skipped when no consumer is active (sample_stamps_enabled == false);
     * total_requests is still bumped via http_server_count_request. */
    http_server_count_request(conn->counters);
    if (req && stamps) {
        req->end_ns = zend_hrtime();
        /* Pass req->end_ns so on_request_sample's CoDel-window logic
         * reuses the stamp instead of taking a fresh zend_hrtime —
         * one syscall less per request on the hot path. */
        http_server_on_request_sample(conn->server,
                                      req->start_ns - req->enqueue_ns,
                                      req->end_ns   - req->start_ns,
                                      req->end_ns);
    }

    zval_ptr_dtor(&retval);
}
/* }}} */

/* {{{ http_handler_coroutine_dispose
 *
 * Guaranteed cleanup — called on normal exit, exception, or cancellation.
 * Handles response sending, request/response destruction, and
 * keep-alive re-arming.
 */
static void http_handler_coroutine_dispose(zend_coroutine_t *coroutine)
{
    http1_request_ctx_t *ctx = (http1_request_ctx_t *)coroutine->extended_data;
    if (ctx == NULL) {
        return;
    }
    http_connection_t *conn = ctx->conn;
    coroutine->extended_data = NULL;

    /* Paired with http_server_on_request_dispatch in
     * http_connection_dispatch_request. Fires on every coroutine
     * teardown path (success, exception, cancellation). */
    http_server_on_request_dispose(conn->counters);

    /* Past this point the coroutine is teardown-bound; clear the
     * back-pointer on the request so a late parse-error path can't
     * ZEND_ASYNC_CANCEL us a second time. */
    if (ctx->request != NULL) {
        ctx->request->coroutine = NULL;
    }
    /* current_request is the conn-level "what's in flight" marker.
     * We are about to retire this dispatch, so clear it now — even
     * though the response is still being formatted/sent below, the
     * request object lifetime is already detached from conn. */
    if (conn->current_request == ctx->request) {
        conn->current_request = NULL;
    }

    /* If the handler threw / was cancelled and the response isn't
     * already committed, derive the response from the exception:
     * Throwable::$code becomes the HTTP status (if it's a valid 4xx
     * /5xx), Throwable::$message becomes the reason / body. Default
     * 500 + "Internal Server Error" for malformed codes or vanilla
     * exceptions without a deliberate HTTP code.
     *
     * This is what the parse-error cancellation path relies on
     * (cancellation exception is HttpException with code/message
     * pre-filled), but it ALSO works for any user-thrown HttpException
     * — `throw new HttpException("Not Found", 404)` from a handler
     * Just Works without further plumbing. */
    bool should_continue = false;
    if (coroutine->exception != NULL
        && !http_response_is_committed(Z_OBJ(ctx->response_zv))) {
        zval rv;
        zend_object *const exc = coroutine->exception;

        zval *code_zv = zend_read_property_ex(exc->ce, exc, ZSTR_KNOWN(ZEND_STR_CODE), 1, &rv);
        const zend_long code = (code_zv != NULL && Z_TYPE_P(code_zv) == IS_LONG)
                                 ? Z_LVAL_P(code_zv) : 0;
        const int status = (code >= 400 && code <= 599) ? (int)code : 500;

        const char *reason = "Internal Server Error";
        zval *msg_zv = zend_read_property_ex(exc->ce, exc, ZSTR_KNOWN(ZEND_STR_MESSAGE), 1, &rv);
        if (msg_zv != NULL && Z_TYPE_P(msg_zv) == IS_STRING && Z_STRLEN_P(msg_zv) > 0) {
            reason = Z_STRVAL_P(msg_zv);
        } else if (status != 500) {
            /* Use a sensible default for non-default statuses without
             * a message. Falling through with "Internal Server Error"
             * for a 414 would be confusing. */
            reason = "";
        }

        http_response_reset_to_error(Z_OBJ(ctx->response_zv), status, reason);
    }

    if (!http_response_is_committed(Z_OBJ(ctx->response_zv))) {
        http_response_set_committed(Z_OBJ(ctx->response_zv));
    }

    /* Inject Alt-Svc (RFC 7838) before drain/format so it rides the
     * same writev as the rest of the headers. NULL when no H3 listener
     * is configured or the env opt-out is set; only set if the handler
     * hasn't supplied its own. */
    {
        zend_string *alt = http_server_get_alt_svc_value(conn->server);
        if (alt != NULL) {
            http_response_set_alt_svc_if_unset(
                Z_OBJ(ctx->response_zv), ZSTR_VAL(alt), ZSTR_LEN(alt));
        }
    }

    /* Graceful drain: append `Connection: close` and transition TCP
     * out of keep-alive (RFC 9112 §9.6, matches HAProxy/Envoy). Checked
     * AFTER set_committed so the formatter sees the final header set,
     * but BEFORE format() so the override actually lands on the wire.
     * A handler-supplied `Connection: keep-alive` is overridden — the
     * RFC grants the server that authority. */
    /* Reuse req->end_ns (already stamped at handler return) so the
     * drain decision skips its own zend_hrtime call. */
    const uint64_t drain_now_ns =
        (ctx->request != NULL && ctx->request->end_ns != 0)
            ? ctx->request->end_ns : zend_hrtime();
    if (http_server_should_drain_now(conn->server, conn, drain_now_ns)) {
        http_response_force_connection_close(Z_OBJ(ctx->response_zv));
        conn->keep_alive = false;
        http_server_on_h1_connection_close_sent(conn->counters);
    }

    /* HTTP/1 format + send. HTTP/2 has its own dispatch path
     * (http2_strategy_dispatch spawns http2_handler_coroutine_*),
     * so this dispose is never reached on an H2 stream.
     *
     * Streaming mode: the handler already
     * wrote headers + chunks on the wire via h1_stream_ops. If they
     * didn't call end(), flush the terminal zero-chunk now; otherwise
     * it was emitted already by mark_ended(). Skip http_response_format
     * entirely — re-serialising would double-commit. */
    conn->state = CONN_STATE_SENDING;
    if (http_response_is_streaming(Z_OBJ(ctx->response_zv))) {
        if (!http_response_is_closed(Z_OBJ(ctx->response_zv))) {
            /* Handler fell through without end() — emit the terminator. */
            (void)http_connection_send(conn, "0\r\n\r\n", 5);
        }
        should_continue = conn->keep_alive;
    } else {
        bool sent;
#ifdef HAVE_OPENSSL
        if (conn->tls != NULL) {
            /* TLS path keeps the legacy single-buffer formatter — the
             * encryption ring needs a contiguous payload, so vectored
             * write would only force an extra copy. */
            zend_string *response_str = http_response_format(Z_OBJ(ctx->response_zv));
            if (response_str) {
                sent = http_connection_send(conn, ZSTR_VAL(response_str),
                                            ZSTR_LEN(response_str));
                zend_string_release(response_str);
            } else {
                sent = false;
            }
        } else
#endif
        {
            /* Threshold branch: writev submit costs ~150ns of fixed
             * overhead (pecalloc + slot loop + completion-cb release
             * loop). For tiny bodies the single-buffer concat formatter
             * is cheaper (memcpy of 2-byte body is essentially free).
             * The crossover sits around 0.5–1 KB; HTTP_WRITEV_THRESHOLD
             * is a conservative cutoff under it. body_len is already a
             * hot load (smart_str header), branch is one cmp + jmp. */
            const size_t body_len =
                    http_response_get_body_len(Z_OBJ(ctx->response_zv));
            if (body_len < HTTP_WRITEV_THRESHOLD) {
                zend_string *response_str =
                        http_response_format(Z_OBJ(ctx->response_zv));
                sent = response_str
                        ? http_connection_send_str_owned(conn, response_str)
                        : false;
            } else {
                /* Large body: skip the concat memcpy; reactor consumes
                 * both refs (headers + body) on completion. */
                zend_string *headers_str = NULL, *body_str = NULL;
                http_response_format_parts(Z_OBJ(ctx->response_zv),
                                           &headers_str, &body_str);
                zend_string *bufs[2];
                unsigned nbufs = 0;
                if (headers_str != NULL && ZSTR_LEN(headers_str) > 0) {
                    bufs[nbufs++] = headers_str;
                } else if (headers_str != NULL) {
                    zend_string_release(headers_str);
                }
                if (body_str != NULL) {
                    bufs[nbufs++] = body_str;
                }
                sent = http_connection_send_strv_owned(conn, bufs, nbufs);
            }
        }
        if (sent) {
            /* Keep-alive is purely a transport decision — it mirrors the
             * request's Connection header (and HTTP version default).
             * http_response_is_closed means "$res->end() was called",
             * which is the *expected* finalization path, not a signal
             * that the transport should close. */
            should_continue = conn->keep_alive;
        }
    }

    /* Tear down per-coroutine state. Zvals + ctx are owned solely by
     * this dispose; no other path looks at them after extended_data
     * was cleared above. */
    zval_ptr_dtor(&ctx->request_zv);
    ZVAL_UNDEF(&ctx->request_zv);
    zval_ptr_dtor(&ctx->response_zv);
    ZVAL_UNDEF(&ctx->response_zv);
    efree(ctx);
    ctx = NULL;

    /* Reset per-request state. Do NOT clear read_buffer_len — any bytes
     * still sitting there belong to the next pipelined request (parser
     * paused at on_message_complete and we shifted the tail to offset
     * 0 in handle_read_completion). */
    conn->request_ready = false;
    conn->headers_complete = false;
    conn->body_complete = false;

    conn->state = should_continue
        ? CONN_STATE_KEEPALIVE_WAIT
        : CONN_STATE_CLOSING;

    /* Reset the conn deadline for the next phase. Going into keep-
     * alive idle wait → keepalive_timeout_ms ahead. Going to close →
     * 0 (the close path destroys this conn directly; the watchdog
     * doesn't need to revisit). ZEND_ASYNC_NOW() reads cached loop
     * time — no syscall on the hot path. */
    if (should_continue && conn->keepalive_timeout_ms > 0) {
        conn->deadline_ms = ZEND_ASYNC_NOW() + conn->keepalive_timeout_ms;
    } else {
        conn->deadline_ms = 0;
    }

    /* Release the dispatch-time pin (see http_connection_dispatch_request).
     * If a competing path requested teardown while we ran (destroy_pending
     * set by the read-callback CLOSING transition under TLS pipelining),
     * finalise it now: skip the resume/keep-alive tail and free conn
     * directly — there is nothing more to do on a connection the peer or
     * a parse error has already retired. */
    ZEND_ASSERT(conn->handler_refcount > 0);
    conn->handler_refcount--;
    if (conn->handler_refcount == 0 && conn->destroy_pending) {
        conn->destroy_pending = false;
        http_connection_destroy(conn);
        return;
    }

#ifdef HAVE_OPENSSL
    /* TLS connections drive their I/O from an event-loop FSM in the
     * read callback (no per-connection coroutine). Hand control back
     * to that FSM: it will either feed a pipelined request out of
     * conn->read_buffer, re-arm the cipher read for the next request,
     * or transition to CLOSING + destroy on a non-keep-alive response. */
    if (conn->tls != NULL) {
        http_connection_tls_resume_after_handler(conn);
        return;
    }
#endif

    /* Drain any pipelined request before honouring a close decision: an
     * EOF observed in read_cb may have flipped keep_alive=false while a
     * pipelined chain is still in the buffer, and clients expect every
     * request they sent to get a response. */
    conn->request_in_flight = false;

    if (conn->read_buffer_len > 0) {
        bool should_destroy = false;
        if (!http_connection_handle_read_completion(conn, &should_destroy)) {
            if (should_destroy) {
                http_connection_destroy(conn);
            }
            return;
        }
    }

    if (!should_continue) {
        http_connection_destroy(conn);
        return;
    }
    /* Multishot reader on conn->io is armed for the connection's lifetime;
     * the next request's bytes will arrive via the read callback. */
}
/* }}} */


/* {{{ http_connection_spawn
 *
 * Lazy coroutine model: no coroutine is created at accept time. We set
 * up conn + scope and arm the first read via the event-loop callback;
 * a handler coroutine spawns only once a full request has been parsed
 * (see http_connection_dispatch_request).
 */
bool http_connection_spawn(const php_socket_t client_fd, zend_async_scope_t *server_scope,
                           zend_fcall_t *handler,
                           const uint32_t read_timeout_ms, const uint32_t write_timeout_ms,
                           const uint32_t keepalive_timeout_ms,
                           http_server_object *server,
                           tls_context_t *tls_ctx)
{
    /* Allocate from the server's slab arena (or ecalloc for the
     * server-less test path). conn->server is set by create. */
    http_connection_t *conn = http_connection_create(client_fd, server_scope, server);
    if (!conn) {
        return false;
    }
    /* Bind hot-path slices — counters / view / log_state pointers. */
    http_server_bind_connection(server, conn);

#ifdef HAVE_OPENSSL
    if (tls_ctx != NULL) {
        conn->tls = tls_session_new(tls_ctx);
        if (conn->tls == NULL) {
            http_connection_destroy(conn);
            return false;
        }
        /* Plaintext queue feeding the cooperative flusher. Freed in
         * http_connection_destroy — each half of the BIO pair must be
         * released independently (BIO_free on one does not free the
         * other). */
        if (BIO_new_bio_pair(&conn->tls_plaintext_bio,
                             HTTP_TLS_PLAINTEXT_RING_BYTES,
                             &conn->tls_plaintext_bio_app,
                             HTTP_TLS_PLAINTEXT_RING_BYTES) != 1) {
            ERR_clear_error();
            conn->tls_plaintext_bio = NULL;
            conn->tls_plaintext_bio_app = NULL;
            http_connection_destroy(conn);
            return false;
        }
        /* TLS records are up to 16 KiB of plaintext. Size read_buffer
         * so one full record lands without a realloc; the growth path
         * kicks in only for pathological pipelines where the parser
         * stalls on a partial message larger than that. */
        if (conn->read_buffer_size < TLS_BIO_RING_SIZE) {
            efree(conn->read_buffer);
            conn->read_buffer_size = TLS_BIO_RING_SIZE;
            conn->read_buffer = emalloc(conn->read_buffer_size);
        }
        conn->state = CONN_STATE_TLS_HANDSHAKE;
    }
#else
    (void)tls_ctx;
#endif

    conn->handler = handler;
    conn->read_timeout_ms = read_timeout_ms;
    conn->write_timeout_ms = write_timeout_ms;
    conn->keepalive_timeout_ms = keepalive_timeout_ms;

    /* server back-pointer set inside http_connection_create. Take a
     * refcount on the C-state — keeps it (and the arena slab memory
     * we just allocated from) alive for late libuv callbacks that
     * may fire after the PHP wrapper goes away. Released in
     * http_connection_destroy. */
    http_server_addref(server);

    /* Initial deadline: waiting for the first request bytes. The
     * per-worker periodic deadline_tick walks the alive list and
     * force-closes any conn where this stamp has elapsed. Uses
     * cached reactor "now" — no syscall. */
    if (read_timeout_ms > 0) {
        conn->deadline_ms = ZEND_ASYNC_NOW() + read_timeout_ms;
    }

    /* Graceful drain state init. If proactive MAX_CONNECTION_AGE is
     * configured, precompute the drain time here (age + ±10% jitter);
     * should_drain_now() will notice on the first response that crosses
     * it. Jitter uses a deterministic hash of the connection pointer so
     * every conn spreads independently without rand() state. */
    conn->created_at_ns       = zend_hrtime();
    conn->drain_pending       = false;
    conn->drain_submitted     = false;
    /* Reactive-drain semantics: a brand-new connection should ignore
     * drain epochs that fired BEFORE it arrived. Otherwise the first
     * CoDel trip "sticks" forever — every future conn sees epoch_seen
     * (0) < epoch_current (N>0), gets drain_pending=true, and ships
     * `Connection: close` on its first response. Surfaced 2026-04-27
     * as a 7.5x H1 RPS regression vs. h2o under wrk t=4 c=100. */
    conn->drain_epoch_seen    = http_server_get_drain_epoch_current(server);
    conn->drain_not_before_ns   = UINT64_MAX;   /* sentinel: no pending drain */
    conn->grace_timer_reserved  = NULL;

    {
        /* Access server->max_connection_age_ns via the accessor —
         * http_server_object layout isn't visible in this TU. */
        const uint64_t base = http_server_get_max_connection_age_ns(server);
        if (base > 0) {
            /* ±10% jitter via deterministic hash of the connection
             * pointer — stable spread across connections without
             * rand() state. */
            const uint64_t h = (uintptr_t)conn * 2654435761ULL;    /* Knuth's multiplicative */
            const uint64_t twenty_pct = base / 5;                  /* 20% of base */
            const uint64_t jitter     = twenty_pct > 0 ? (h % twenty_pct) : 0;
            const uint64_t offset     = (base - twenty_pct / 2) + jitter;  /* [base-10%, base+10%] */
            conn->drain_not_before_ns = conn->created_at_ns + offset;
        }
    }

    /* Use the server scope directly. We previously minted a per-conn
     * child scope here so cancellation could be scoped per-connection,
     * but in the event-loop read model there's no coroutine alive in
     * the scope between requests, and the orphaned per-conn scopes
     * leaked across server start/stop cycles. The handler coroutine
     * still runs under server_scope so server stop still cancels it. */
    conn->scope = server_scope;

#ifdef HAVE_OPENSSL
    if (conn->tls != NULL) {
        /* Event-driven TLS: arm a one-shot ciphertext read; the first
         * chunk fires the read callback and the FSM drives the
         * handshake from CONN_STATE_TLS_HANDSHAKE. No per-connection
         * coroutine — handler coroutines spawn per request, exactly
         * like the plaintext path. Multishot is left disabled for
         * TLS so each chunk can land directly in a fresh BIO slot
         * without a staging buffer. alloc_cb must be cleared too:
         * libuv_io_alloc_cb honors alloc_cb unconditionally and
         * would route ciphertext into the plaintext read_buffer
         * while tls_commit_cipher_in advances the BIO write head as
         * if those bytes had landed in the ring — SSL_do_handshake
         * then reads zero-init garbage and alerts decode_error. */
        ZEND_ASYNC_IO_CLR_MULTISHOT(conn->io);
        conn->io->alloc_cb = NULL;
        return http_connection_tls_arm_read(conn);
    }
#endif

    /* Kick off the first read. http_connection_read handles its own cleanup on
     * failure. */
    (void)http_connection_read(conn);
    return true;
}
/* }}} */
