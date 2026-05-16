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
#include "Zend/zend_hrtime.h"
#include "Zend/zend_async_API.h"
#include "php_http_server.h"     /* http_response_get_* + http_connection_send */
#include "http_protocol_strategy.h"
#include "http_connection.h"
#include "core/http_connection_internal.h"
#include "http2/http2_session.h"
#include "http2/http2_stream.h"
#include "http2/http2_static_response.h"

#ifdef HAVE_OPENSSL
#include <openssl/bio.h>
#endif
#include "http1/http_parser.h"   /* http_request_t */
#include "static/static_handler.h"
#include "http_send_file.h"
#include "http_response_internal.h"

#include <string.h>

/* MSG_NOSIGNAL is a POSIX flag that suppresses SIGPIPE on send() to a
 * half-closed socket. Windows has no SIGPIPE — Winsock signals errors
 * via the return value (WSAECONNRESET / WSAECONNABORTED) — so the flag
 * is unnecessary there and absent from <winsock2.h>. Define it to 0 on
 * Windows so call-sites stay portable. */
#ifndef MSG_NOSIGNAL
# define MSG_NOSIGNAL 0
#endif

/* Defined in src/http_request.c — builds a PHP HttpRequest zval from
 * a parsed http_request_t. Declared extern here (not in the public
 * header) to mirror the HTTP/1 dispatch path in http_connection.c. */
extern zval *http_request_create_from_parsed(http_request_t *req);

/*
 * HTTP/2 strategy — bridge between the connection layer's vtable and
 * the nghttp2-backed session in http2_session.c.
 *
 * Design invariants:
 *  - Strategy owns its http2_session_t. Session is lazy on first
 *    feed() and freed in cleanup(). No HTTP/2 concept leaks outside
 *    src/http2/: the connection layer only sees http_protocol_strategy_t*.
 *  - Dispatch spawns a per-stream handler coroutine whose
 *    extended_data is the http2_stream_t*. Concurrent streams keep
 *    their own request_zv/response_zv — multiplex stays safe.
 *  - http2_strategy_commit_response is called directly from the
 *    connection's handler-dispose path (via the HAVE_HTTP2 branch in
 *    http_handler_coroutine_dispose). It serialises the PHP
 *    $response into HEADERS + DATA frames and drains them into the
 *    socket through http_connection_send.
 */

/* Per-strategy state. Kept file-local so nothing outside this TU can
 * reach it; the connection layer only keeps a http_protocol_strategy_t*. */
typedef struct {
    http_protocol_strategy_t base;
    http2_session_t         *session;   /* lazy — created on first feed() */
    http_connection_t       *conn;      /* captured at session creation for the trampoline */
} http2_strategy_t;

/* Forward declarations for the H2 handler coroutine lifecycle — the
 * full bodies live below, strategy code refers to them by name. */
static void http2_handler_coroutine_entry(void);
static void http2_handler_coroutine_dispose(zend_coroutine_t *coroutine);

/* Commit a stream's response once its handler finished. Lives here
 * instead of being reachable from http_connection.c because the
 * dispatch is now fully H2-internal. */
static bool http2_commit_stream_response(http_connection_t *conn,
                                         http2_stream_t *stream);

/* Streaming vtable exported via http_response_install_stream_ops.
 * Forward-declared so dispatch can take its address. */
extern const http_response_stream_ops_t h2_stream_ops;

/* Forward decl so dispose can call mark_ended on a stream whose
 * handler skipped $res->end(). Defined further down. */
static void h2_stream_mark_ended(void *ctx);

/* Suspend the current handler coroutine until either
 *  - stream->write_event fires (cb_on_frame_recv saw WINDOW_UPDATE
 *    and opened the flow-control window for us), or
 *  - write_timeout elapses (peer stuck — connection tear-down
 *    follows).
 * Returns true on woken-by-event, false on timeout / cancellation
 * (PHP exception is set in the latter case). */
static bool h2_wait_for_drain_event(http2_stream_t *stream,
                                    http_connection_t *conn);

/* Ownership ctx for send_batched_writev — free_cb releases body refs +
 * iov + emit_buf + ctx. */
typedef struct {
    char             *emit_buf;
    zend_async_buf_t *iov;
    zend_refcounted **body_refs;
    unsigned          body_refs_cnt;
} h2_writev_ctx_t;

static void h2c_writev_free_cb(void *user_data, zend_async_io_t *io);

/* === Static-handler dispatch callbacks ============================
 *
 * The protocol-agnostic static FSM in src/static/http_static.c calls
 * back through these to manage protocol-side bookkeeping (refcounts,
 * counters, eventual cleanup). Mirrors h1_static_dispatch_cbs in
 * src/core/http_connection.c, but the user pointer is the
 * http2_stream_t and the lifecycle hooks differ:
 *   - HARD_ZERO arms by pinning conn->handler_refcount + bumping the
 *     stream refcount so the close hook can run after dispose.
 *   - on_static_done releases the conn pin and lets the stream clean
 *     up via http2_stream_release; no PHP-side coroutine to dispose.
 *   - on_passthrough_to_php is unused (H2 currently doesn't expose
 *     on_missing:Next on this code path — falling back to PHP from
 *     a stream that armed-then-rolled is a follow-up). */
static void h2_static_on_hard_zero_armed(void *user)
{
    http2_stream_t *stream = (http2_stream_t *)user;
    http_connection_t *conn = http2_session_get_conn(stream->session);

    conn->handler_refcount++;
    http_server_on_request_dispatch(conn->counters);

    /* Pin the stream itself so cb_on_stream_close + the static FSM
     * close hook can fire after the dispatch frame returns. Released
     * in h2_static_on_static_done. */
    stream->refcount++;
}

static void h2_static_on_static_done(void *user, int status)
{
    (void)status;
    http2_stream_t *stream = (http2_stream_t *)user;
    http_connection_t *conn = http2_session_get_conn(stream->session);

    http_server_on_request_dispose(conn->counters);

    if (conn->handler_refcount > 0) {
        conn->handler_refcount--;
    }

    http_connection_destroy_if_idle_deferred(conn);

    /* Released by stream_close — drop the dispatch-side ref taken in on_hard_zero_armed. */
    http2_stream_release(stream);
}

/* H2 multiplexes on one TCP connection; Connection / Keep-Alive are
 * filtered out of every response anyway. Always keep-alive. */
static bool h2_static_keep_alive(void *user)
{
    (void)user;
    return true;
}

static const http_static_dispatch_cbs_t h2_static_dispatch_cbs = {
    .on_armed    = h2_static_on_hard_zero_armed,
    .on_done        = h2_static_on_static_done,
    .on_passthrough = NULL,
    .keep_alive            = h2_static_keep_alive,
};

/* Session calls this on HEADERS + END_HEADERS for each new stream.
 * Per plan §3.6 this fires regardless of END_STREAM so gRPC bidi
 * handlers start running before the body arrives.
 *
 * Spawns a per-stream handler coroutine whose extended_data is the
 * http2_stream_t*. Each stream's request_zv / response_zv live on
 * the stream, so concurrent streams can't trample each other —
 * unlike HTTP/1's one-request-per-connection generic on_request_ready
 * path. */
static void http2_strategy_dispatch(struct http_request_t *request,
                                    const uint32_t stream_id,
                                    void *user_data)
{
    (void)request;      /* Reachable via stream->request; we use that path. */
    http2_strategy_t *self = (http2_strategy_t *)user_data;

    http2_stream_t *stream = http2_session_find_stream(self->session,
                                                             stream_id);

    if (stream == NULL || self->conn == NULL) {
        return;
    }
    /* Static-only deployments register a static mount but no PHP
     * handler — the static dispatch path below claims the request
     * before we ever check conn->handler. The PHP-handler-required
     * branches are guarded individually further down. */
    const bool has_static_mount =
        http_static_handler_count(self->conn->server) > 0;

    if (self->conn->handler == NULL && !has_static_mount) {
        return;
    }

    /* Build the PHP HttpRequest object from the parsed request. The
     * object takes ownership of the http_request_t — stream_free's
     * `request_dispatched` guard already knows not to free it again. */
    zval *req_obj = http_request_create_from_parsed(stream->request);
    ZVAL_COPY_VALUE(&stream->request_zv, req_obj);
    efree(req_obj);

    object_init_ex(&stream->response_zv, http_response_ce);
    http_response_set_protocol_version(Z_OBJ(stream->response_zv),
                                       self->conn->http_version);

    /* Let HttpResponse::send() reach this stream's chunk queue via
     * the vtable. Ops installed once at dispatch;
     * streaming mode is a handler opt-in (only activated when send()
     * is actually called). */
    http_response_install_stream_ops(Z_OBJ(stream->response_zv),
                                     &h2_stream_ops, stream);

#ifdef HAVE_HTTP_COMPRESSION
    /* Attach compression state (issue #8). Mirror of the H1 dispatch
     * hook — uses conn->config cached at bind time. */
    if (self->conn->config != NULL) {
        extern void http_compression_attach(zend_object *,
            http_request_t *, http_server_config_t *);
        http_compression_attach(Z_OBJ(stream->response_zv),
                                stream->request, self->conn->config);
    }
#endif
    /* Per-request JSON encode default for HttpResponse::json(). */
    if (self->conn->config != NULL) {
        extern void http_response_set_default_json_flags(zend_object *, uint32_t);
        http_response_set_default_json_flags(
            Z_OBJ(stream->response_zv), self->conn->config->json_encode_flags);
    }

    /* Static-handler dispatch (issue #13). Identical policy to the
     * H1 site in http_connection_dispatch_request:
     *   HARD_ZERO   — FSM owns the request; on_hard_zero_armed has
     *                 pinned conn + stream. Return without spawning a
     *                 coroutine; on_static_done eventually unpins.
     *   HANDLED     — response_obj populated synchronously (small
     *                 4xx body / soft skip). Mark stream->skip_handler
     *                 so http2_handler_coroutine_entry skips the user
     *                 handler; dispose still runs the regular
     *                 buffered-commit path so the wire frames go out.
     *   PASSTHROUGH — no mount matched; spawn the user handler. */
    if (UNEXPECTED(has_static_mount)) {
        const http_static_result_t static_rc =
            http_static_try_serve(self->conn->server, stream->request,
                                  Z_OBJ(stream->response_zv),
                                  self->conn->counters,
                                  &h2_static_dispatch_cbs, stream);

        if (static_rc == HTTP_STATIC_HARD_ZERO) {
            return;
        }

        if (static_rc == HTTP_STATIC_HANDLED) {
            stream->skip_handler = true;
        }
    }

    /* No PHP handler and the static path didn't claim the request:
     * synthesise a 404 on the response so the dispose-side commit
     * sends one. Otherwise the coroutine entry's handler==NULL
     * guard would silently return and the stream would hang until
     * the client times out. Mirrors the H1 path in
     * http_connection_dispatch_request. */
    if (self->conn->handler == NULL && !stream->skip_handler) {
        http_response_static_set_status(Z_OBJ(stream->response_zv), 404);
        http_response_static_set_header(Z_OBJ(stream->response_zv),
            "content-type", 12, "text/plain; charset=utf-8", 25);
        zend_string *msg = zend_string_init("Not Found", 9, 0);
        http_response_static_set_body_str(Z_OBJ(stream->response_zv), msg);
        zend_string_release(msg);
        stream->skip_handler = true;
    }

    /* Spawn the handler coroutine. extended_data is the STREAM, not
     * the connection — that's what makes multiplex safe: N
     * coroutines hold N distinct stream pointers, each pointing at
     * its own zvals. */
    zend_coroutine_t *co = ZEND_ASYNC_NEW_COROUTINE(self->conn->scope);

    if (co == NULL) {
        zval_ptr_dtor(&stream->request_zv);
        ZVAL_UNDEF(&stream->request_zv);
        zval_ptr_dtor(&stream->response_zv);
        ZVAL_UNDEF(&stream->response_zv);
        return;
    }

    co->internal_entry    = http2_handler_coroutine_entry;
    co->extended_data     = stream;
    co->extended_dispose  = http2_handler_coroutine_dispose;

    /* Save for cancellation (peer RST_STREAM, server shutdown). */
    stream->coroutine          = co;
    stream->request->coroutine = co;

    if (http_server_sample_stamps_enabled(self->conn->view)) {
        stream->request->enqueue_ns = zend_hrtime();
    }

    /* Bracket on the server's in-flight counter (paired with
     * http_server_on_request_dispose in http2_handler_coroutine_dispose).
     * Each H2 stream is a distinct logical request, so every stream
     * contributes one unit — CoDel/admission see H2 load at the right
     * granularity. */
    http_server_on_request_dispatch(self->conn->counters);

    /* Bump refcount — coroutine now holds its own reference. The
     * session's stream-table reference goes away when
     * on_stream_close_cb fires; our coroutine-held reference
     * survives until dispose returns. That decouples nghttp2's
     * close-during-drain from our handler-dispose lifecycle. */
    stream->refcount++;

    /* Bump the connection-level handler refcount too. Prevents the
     * read-callback peer-FIN path from freeing conn/strategy while
     * this handler is still running (see http_connection_destroy). */
    if (self->conn != NULL) {
        self->conn->handler_refcount++;
    }

    ZEND_ASYNC_ENQUEUE_COROUTINE(co);
}

/* -------------------------------------------------------------------------
 * Handler coroutine — HTTP/2-specific path.
 *
 * Mirrors the HTTP/1 http_handler_coroutine_entry/_dispose pair in
 * src/core/http_connection.c, but keyed on a stream instead of a
 * connection. One coroutine per stream → multiplex works.
 * ------------------------------------------------------------------------- */

/* Invokes the user handler with the stream's own (request, response)
 * zvals. All state lives on the stream — multiplexed coroutines
 * never touch a shared conn-level zval. */
static void http2_handler_coroutine_entry(void)
{
    const zend_coroutine_t *co = ZEND_ASYNC_CURRENT_COROUTINE;
    http2_stream_t *stream = (http2_stream_t *)co->extended_data;
    http_connection_t *conn = http2_session_get_conn(stream->session);

    /* Static-handler HANDLED path: response_obj already carries the
     * synchronous 4xx error body. Skip the user handler entirely;
     * dispose runs the normal buffered commit and the wire frames go
     * out the same way they would for any handler-set response. */
    if (stream->skip_handler) {
        http_server_count_request(conn->counters);
        return;
    }

    if (conn->handler == NULL) { return; }

    const bool stamps = http_server_sample_stamps_enabled(conn->view);

    if (stream->request != NULL && stamps) {
        stream->request->start_ns = zend_hrtime();
    }

#ifdef HAVE_HTTP_COMPRESSION
    /* Inbound Content-Encoding decode (issue #8). Mirror of the H1
     * handler-entry hook — produces a canned error response and skips
     * the user handler when decoding fails. */
    if (stream->request != NULL) {
        extern int http_compression_decode_request_body(
            http_request_t *, http_server_config_t *);
        extern void http_response_set_error(zend_object *, int, const char *);
        int dec = http_compression_decode_request_body(
            stream->request, conn->config);

        if (dec != 0) {
            http_response_set_error(Z_OBJ(stream->response_zv), dec,
                dec == 415 ? "Unsupported Content-Encoding" :
                dec == 413 ? "Payload Too Large after decompression" :
                             "Malformed compressed request body");
            http_server_count_request(conn->counters);

            if (stamps) stream->request->end_ns = zend_hrtime();
            return;
        }
    }
#endif

    zval params[2], retval;
    ZVAL_COPY_VALUE(&params[0], &stream->request_zv);
    ZVAL_COPY_VALUE(&params[1], &stream->response_zv);
    ZVAL_UNDEF(&retval);

    zend_fcall_info fci = {
        .size           = sizeof(zend_fcall_info),
        .function_name  = conn->handler->fci.function_name,
        .retval         = &retval,
        .params         = params,
        .object         = NULL,
        .param_count    = 2,
        .named_params   = NULL,
    };
    /* Bailout firewall — see http_handler_log_bailout in
     * src/core/http_connection.c for the rationale. Without it any
     * zend_bailout under the user handler propagates to
     * async_coroutine_execute's outer zend_catch and quietly retires the
     * worker via should_start_graceful_shutdown. */
    volatile bool bailout = false;
    zend_try
    {
        zend_call_function(&fci, &conn->handler->fci_cache);
    }

    zend_catch
    {
        bailout = true;
    }

    zend_end_try();

    if (UNEXPECTED(bailout)) {
        const char *m = (stream->request && stream->request->method)
                            ? ZSTR_VAL(stream->request->method) : "?";
        const char *u = (stream->request && stream->request->uri)
                            ? ZSTR_VAL(stream->request->uri) : "?";
        http_handler_log_bailout("h2", co, m, u);
        /* Skip retval dtor + sample + count — post-bailout PHP state
         * cannot sustain those. Stream dispose still runs and will
         * RST or send whatever the response was at the point of bailout. */
        return;
    }

    /* Stamp end_ns + feed backpressure sample BEFORE retval dtor so
     * destructor time on a returned object doesn't count as service
     * time. Matches the HTTP/1 handler-coroutine discipline. Stamps and
     * the sample call are skipped when no consumer (CoDel/telemetry) is
     * active; total_requests is still bumped. */
    http_server_count_request(conn->counters);

    if (stream->request != NULL && stamps) {
        stream->request->end_ns = zend_hrtime();
        http_server_on_request_sample(
            conn->server,
            stream->request->start_ns - stream->request->enqueue_ns,
            stream->request->end_ns   - stream->request->start_ns,
            stream->request->end_ns);
    }

    zval_ptr_dtor(&retval);
}

/* Post-handler: serialise the response into HEADERS + DATA (+
 * trailers) frames and drain to the socket. See the body below for
 * the full commit-response dance — extracted so the dispose path is
 * small and easy to reason about. */
static bool http2_commit_stream_response(http_connection_t *conn,
                                         http2_stream_t *stream);

/* HttpResponse::sendFile() — H2 dispose hand-off (issue #13). Hijacks
 * dispose: pin stream + conn, kick the sendfile FSM, defer the
 * response-zval dtor + refcount drop tail to h2_sendfile_on_done. */
static void h2_sendfile_arm(http_connection_t *conn, http2_stream_t *stream);

/* Guaranteed cleanup — runs on normal return, exception, or
 * cancellation (peer RST_STREAM / server stop). The stream remains
 * in nghttp2's table until on_stream_close_cb tears it down; we
 * only release the PHP-side state here. */
static void http2_handler_coroutine_dispose(zend_coroutine_t *coroutine)
{
    http2_stream_t *stream = (http2_stream_t *)coroutine->extended_data;

    /* Break the back-pointer BEFORE anything else so a late
     * RST_STREAM handler can't re-enter ZEND_ASYNC_CANCEL on a
     * coroutine that's already tearing down. Same discipline as the
     * HTTP/1 coroutine dispose path. */
    stream->coroutine = NULL;

    if (stream->request != NULL) {
        stream->request->coroutine = NULL;
    }

    http_connection_t *conn = http2_session_get_conn(stream->session);

    /* Paired with http_server_on_request_dispatch in
     * http2_strategy_dispatch. conn->counters is always non-NULL (dummy
     * fallback), so the inline bump is safe regardless of conn state. */
    if (conn != NULL) {
        http_server_on_request_dispose(conn->counters);
    }

    /* If the handler threw and never committed, derive a response
     * from the exception (code → status, message → body). Same
     * policy as the HTTP/1 dispose path in
     * http_handler_coroutine_dispose — kept in sync so
     * HttpException and parse-error cancellation behave identically
     * on both protocols. */
    if (coroutine->exception != NULL && conn != NULL &&
        !Z_ISUNDEF(stream->response_zv) &&
        !http_response_is_committed(Z_OBJ(stream->response_zv))) {
        zval rv, *code_zv, *msg_zv;
        zend_object *exc = coroutine->exception;

        code_zv = zend_read_property_ex(exc->ce, exc,
                                        ZSTR_KNOWN(ZEND_STR_CODE), 1, &rv);
        const zend_long code = (code_zv != NULL && Z_TYPE_P(code_zv) == IS_LONG)
                                 ? Z_LVAL_P(code_zv) : 0;
        const int status = (code >= 400 && code <= 599) ? (int)code : 500;

        const char *reason = "Internal Server Error";
        msg_zv = zend_read_property_ex(exc->ce, exc,
                                       ZSTR_KNOWN(ZEND_STR_MESSAGE), 1, &rv);

        if (msg_zv != NULL && Z_TYPE_P(msg_zv) == IS_STRING && Z_STRLEN_P(msg_zv) > 0) {
            reason = Z_STRVAL_P(msg_zv);
        } else if (status != 500) {
            reason = "";
        }

        http_response_reset_to_error(Z_OBJ(stream->response_zv), status, reason);
    }

    if (!Z_ISUNDEF(stream->response_zv) &&
        !http_response_is_committed(Z_OBJ(stream->response_zv))) {
        http_response_set_committed(Z_OBJ(stream->response_zv));
    }

    /* sendFile() handoff (issue #13). Take the descriptor before
     * the streaming/buffered branch so the FSM owns delivery; the
     * regular zval-dtor + refcount tail runs from h2_sendfile_on_done
     * after the FSM finalizes. Return early — dispose will resume
     * via on_done. */
    if (!Z_ISUNDEF(stream->response_zv) && conn != NULL
        && http_response_has_send_file(Z_OBJ(stream->response_zv))) {
        h2_sendfile_arm(conn, stream);
        return;
    }

    /* Streaming: skip buffered commit (would RST); just mark ended for EOF. */
    const bool is_streaming = !Z_ISUNDEF(stream->response_zv)
                              && http_response_is_streaming(Z_OBJ(stream->response_zv));

    if (is_streaming) {
        if (!stream->streaming_ended) {
            h2_stream_mark_ended(stream);
        }
    } else if (conn != NULL && !Z_ISUNDEF(stream->response_zv)) {
        (void)http2_commit_stream_response(conn, stream);
    }

    /* zvals must outlive dispose: data_provider borrows response_body
     * past dispose (emit runs later from scheduler context). */
    http2_stream_release(stream);

    if (conn != NULL) {
        if (conn->handler_refcount > 0) {
            conn->handler_refcount--;
        }

        http_connection_destroy_if_idle_deferred(conn);
    }
}

typedef struct {
    http_connection_t *conn;
    http2_stream_t    *stream;
} h2_sendfile_user_t;

static void h2_sendfile_on_done(void *user, int status)
{
    (void)status;
    h2_sendfile_user_t *u = (h2_sendfile_user_t *)user;
    http_connection_t *conn = u->conn;
    http2_stream_t *stream = u->stream;
    efree(u);

    /* Mirror the original dispose tail: dtor zvals, release stream,
     * release conn handler ref. Counters were retired in dispose. */
    if (!Z_ISUNDEF(stream->request_zv)) {
        zval_ptr_dtor(&stream->request_zv);
        ZVAL_UNDEF(&stream->request_zv);
    }

    if (!Z_ISUNDEF(stream->response_zv)) {
        zval_ptr_dtor(&stream->response_zv);
        ZVAL_UNDEF(&stream->response_zv);
    }

    http2_stream_release(stream);

    if (conn != NULL) {
        if (conn->handler_refcount > 0) {
            conn->handler_refcount--;
        }

        http_connection_destroy_if_idle_deferred(conn);
    }
}

static void h2_sendfile_arm(http_connection_t *conn, http2_stream_t *stream)
{
    /* Counters paired with the original dispatch's on_request_dispatch.
     * h2_handler_coroutine_dispose's tail would have done this; do it
     * now so dispose's early-return doesn't leak the in-flight count. */
    if (conn->counters != NULL) {
        http_server_on_request_dispose(conn->counters);
    }

    http_send_file_request_t *sf_req =
        http_response_take_send_file(Z_OBJ(stream->response_zv));

    if (sf_req == NULL) {
        /* Race / accounting bug — fall through to commit. */
        (void)http2_commit_stream_response(conn, stream);
        zval_ptr_dtor(&stream->request_zv);
        ZVAL_UNDEF(&stream->request_zv);
        zval_ptr_dtor(&stream->response_zv);
        ZVAL_UNDEF(&stream->response_zv);
        http2_stream_release(stream);

        if (conn->handler_refcount > 0) conn->handler_refcount--;

        http_connection_destroy_if_idle_deferred(conn);

        return;
    }

    h2_sendfile_user_t *u = ecalloc(1, sizeof(*u));
    u->conn   = conn;
    u->stream = stream;

    if (!http_send_file_dispatch(stream->request, Z_OBJ(stream->response_zv),
                                  sf_req, h2_sendfile_on_done, u)) {
        /* Dispatch failed — response now carries a 500. Commit the
         * synthesized error response, then run the standard tail. */
        efree(u);
        (void)http2_commit_stream_response(conn, stream);
        zval_ptr_dtor(&stream->request_zv);
        ZVAL_UNDEF(&stream->request_zv);
        zval_ptr_dtor(&stream->response_zv);
        ZVAL_UNDEF(&stream->response_zv);
        http2_stream_release(stream);

        if (conn->handler_refcount > 0) conn->handler_refcount--;

        http_connection_destroy_if_idle_deferred(conn);
    }
}

static int http2_feed(http_protocol_strategy_t *strategy,
                      http_connection_t *conn,
                      const char *data,
                      size_t len,
                      size_t *consumed_out)
{
    http2_strategy_t *self = (http2_strategy_t *)strategy;

    /* Lazily spin up the session on first feed. http2_session_new
     * submits initial SETTINGS + bumps the connection window, so by
     * the time this call returns, nghttp2 already has bytes queued
     * that the connection layer will drain. */
    if (self->session == NULL) {
        self->conn = conn;
        self->session = http2_session_new(conn,
                                          http2_strategy_dispatch, self);

        if (self->session == NULL) {
            if (consumed_out != NULL) { *consumed_out = 0; }
            return -1;
        }
    }

    const int rc = http2_session_feed(self->session, data, len, consumed_out);

    if (rc < 0) {
        /* Error-path drain. When feed() flagged a bad connection preface
         * or a hard protocol violation, the session may have queued a
         * GOAWAY(PROTOCOL_ERROR) before giving up. Flush it synchronously
         * so the peer sees the diagnostic frame before we drop TCP —
         * otherwise it observes an unexplained EOF, which is non-compliant
         * per RFC 9113 §3.4 and fails strict conformance tools.
         *
         * TLS path is unaffected here: terminate_session + drain happens
         * inside the TLS coroutine's own write pipeline, not ours. */
        if (conn == NULL) {
            return rc;  /* Offline unit-test fixtures pass NULL conn. */
        }
        /* Plaintext-only error drain. On TLS the GOAWAY goes through
         * the TLS coroutine's own encrypt+write pipeline; writing raw
         * plaintext to the socket from here would corrupt the stream. */
        bool should_drain = conn->io != NULL;
#ifdef HAVE_OPENSSL
        should_drain = should_drain && conn->tls == NULL;
#endif
        if (should_drain) {
            const php_socket_t fd = (php_socket_t)conn->io->descriptor.socket;

            if (fd != (php_socket_t)-1) {
                /* Bad-preface path: nghttp2 can't queue a GOAWAY itself
                 * once BAD_CLIENT_MAGIC has fired. Write the static
                 * template directly. */
                if (http2_session_should_emit_bad_preface_goaway(self->session)) {
                    (void)send(fd,
                               http2_session_bad_preface_goaway_bytes(),
                               HTTP2_BAD_PREFACE_GOAWAY_LEN,
                               MSG_NOSIGNAL);
                } else {
                    char drain_buf[256];
                    const ssize_t n = http2_session_drain(self->session,
                                                           drain_buf,
                                                           sizeof(drain_buf));

                    if (n > 0) {
                        (void)send(fd, drain_buf, (size_t)n, MSG_NOSIGNAL);
                    }
                }
            }
        }

        return rc;
    }

    /* Flush any control frames nghttp2 queued as a reaction to the
     * bytes we just fed — initial SETTINGS on first feed, SETTINGS
     * ACK, PING ACK, WINDOW_UPDATE, GOAWAY on errors, etc.
     *
     * Without this, a peer that sends preface + SETTINGS and waits
     * for server SETTINGS (h2spec, nghttp-without-request) deadlocks:
     * we've parsed its frames but never emit ours until a handler
     * commits a response. Curl doesn't notice because it sends the
     * first request back-to-back with the preface, and the response-
     * commit path drains everything together. Conformance tools are
     * pickier.
     *
     * Context discipline:
     *  - Plaintext h2c: http2_feed runs from the read-callback
     *    (scheduler context — cannot suspend). Use raw non-blocking
     *    send() for control frames. These are small (SETTINGS 30 B,
     *    ACK 9 B, WINDOW_UPDATE 13 B, PING 17 B) and fit in the
     *    kernel send buffer virtually always.
     *  - TLS h2: http2_feed runs from the TLS coroutine, which has
     *    its own drain pipeline that flushes plaintext → BIO →
     *    ciphertext → socket after decrypt-and-process loops. We
     *    MUST NOT double-drain from here — injecting writes into
     *    the TLS state mid-loop corrupts OpenSSL's internal buffers.
     *    Skip the eager drain for TLS; control frames go out on the
     *    TLS coroutine's next natural write tick, which happens
     *    before the next read (plenty fast for h2spec-style timing).
     *  - Peer-RST-mid-stream paths (phpt 077) likewise rely on the
     *    nghttp2 callback + ZEND_ASYNC_CANCEL path that happens
     *    WITHIN session_feed; we must not pre-empt with a raw send
     *    before the cancel has propagated.
     *
     * So: only drain eagerly when the connection is plaintext AND
     * it isn't in a coroutine context already (coroutine callers
     * will hit their own downstream drain loops). */
#ifdef HAVE_OPENSSL
    if (conn->tls != NULL) {
        return rc;
    }
#endif

    /* If we're already in a coroutine (e.g. a re-entry from the
     * handler path), skip — the caller owns the drain. */
    if (!ZEND_ASYNC_IS_SCHEDULER_CONTEXT
        && ZEND_ASYNC_CURRENT_COROUTINE != NULL) {
        return rc;
    }

    char drain_buf[16384];
    while (http2_session_want_write(self->session)) {
        const ssize_t n = http2_session_drain(self->session,
                                              drain_buf, sizeof(drain_buf));

        if (n <= 0) { break; }

        if (conn->io == NULL) { break; }
        const php_socket_t fd = (php_socket_t)conn->io->descriptor.socket;

        if (fd == (php_socket_t)-1) { break; }
        const ssize_t sent = send(fd, drain_buf, (size_t)n, MSG_NOSIGNAL);

        if (sent != (ssize_t)n) { break; }
    }

    /* Session fully winding down (e.g. nghttp2 auto-submitted a
     * GOAWAY(PROTOCOL_ERROR) on an invalid inbound frame). After the
     * drain above we've flushed everything nghttp2 had; keeping the
     * TCP channel open would leave the peer hanging until its own
     * timeout. Signal the connection layer to tear down by returning
     * a non-zero rc — the error path already closes the socket. */
    if (!http2_session_want_read(self->session) &&
        !http2_session_want_write(self->session)) {
        return -1;
    }

    return rc;
}

/* Commit a response on a specific stream. Per-stream dispatch means
 * we already KNOW which stream, no scan needed.
 * Extracts status / headers / body / trailers from the PHP
 * HttpResponse, submits via http2_session_submit_response +
 * http2_session_submit_trailer, then drains HEADERS + DATA +
 * optional HEADERS(trailers) into the socket through
 * http_connection_send. */
static bool http2_commit_stream_response(http_connection_t *conn,
                                         http2_stream_t *stream)
{
    if (conn == NULL || conn->strategy == NULL || stream == NULL ||
        Z_ISUNDEF(stream->response_zv)) {
        return false;
    }

    zend_object *response_obj = Z_OBJ(stream->response_zv);
    http2_strategy_t *self = (http2_strategy_t *)conn->strategy;

    if (self->session == NULL) {
        return false;
    }

#ifdef HAVE_HTTP_COMPRESSION
    /* H2 reads body via http_response_get_body() directly rather than
     * http_response_format[/_parts], so the buffered apply hook lives
     * here too — must run before the headers flatten so the mutated
     * Content-Encoding/Vary ride the HEADERS frame. */
    {
        extern void http_compression_apply_buffered(zend_object *);
        http_compression_apply_buffered(response_obj);
    }
#endif

    /* Advertise H3 endpoint to H2 clients via Alt-Svc.
     * Same hook as H1; no-op when handler already set the header or
     * no H3 listener exists. Injected before the header-flatten so
     * the value rides the HEADERS frame. */
    {
        zend_string *alt = http_server_get_alt_svc_value(conn->server);

        if (alt != NULL) {
            http_response_set_alt_svc_if_unset(
                response_obj, ZSTR_VAL(alt), ZSTR_LEN(alt));
        }
    }

    /* Flatten response headers into http2_header_view_t[]. Two-pass:
     * first count admissible (name, value) pairs, then allocate exactly
     * that many — no arbitrary multiplier, no silent truncation for
     * headers with many values (e.g. multiple Set-Cookie). nghttp2
     * copies name/value into its HPACK dynamic table so the backing
     * zend_string memory only needs to live through this call. */
    HashTable *headers = http_response_get_headers(response_obj);

    size_t total_values = 0;

    if (headers != NULL) {
        zend_string *name;
        zval        *values;
        ZEND_HASH_FOREACH_STR_KEY_VAL(headers, name, values) {
            if (name == NULL)                                            continue;

            if (!http_response_header_allowed_h2h3(ZSTR_VAL(name), ZSTR_LEN(name))) continue;

            if (EXPECTED(Z_TYPE_P(values) == IS_STRING)) {
                total_values++;
            } else if (Z_TYPE_P(values) == IS_ARRAY) {
                zval *val;
                ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(values), val) {
                    if (Z_TYPE_P(val) == IS_STRING) { total_values++; }
                } ZEND_HASH_FOREACH_END();
            }
        } ZEND_HASH_FOREACH_END();
    }

    http2_header_view_t scratch[HTTP2_NV_SCRATCH];
    http2_header_view_t *nv_view = scratch;
    http2_header_view_t *nv_heap = NULL;

    if (total_values > HTTP2_NV_SCRATCH) {
        nv_heap = emalloc(total_values * sizeof(*nv_heap));
        nv_view = nv_heap;
    }

    size_t nv_count = 0;

    if (headers != NULL) {
        zend_string *name;
        zval        *values;
        ZEND_HASH_FOREACH_STR_KEY_VAL(headers, name, values) {
            if (name == NULL)                                            continue;

            if (!http_response_header_allowed_h2h3(ZSTR_VAL(name), ZSTR_LEN(name))) continue;

            if (EXPECTED(Z_TYPE_P(values) == IS_STRING)) {
                nv_view[nv_count].name      = ZSTR_VAL(name);
                nv_view[nv_count].name_len  = ZSTR_LEN(name);
                nv_view[nv_count].value     = Z_STRVAL_P(values);
                nv_view[nv_count].value_len = Z_STRLEN_P(values);
                nv_count++;
            } else if (Z_TYPE_P(values) == IS_ARRAY) {
                zval *val;
                ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(values), val) {
                    if (Z_TYPE_P(val) != IS_STRING) { continue; }
                    nv_view[nv_count].name      = ZSTR_VAL(name);
                    nv_view[nv_count].name_len  = ZSTR_LEN(name);
                    nv_view[nv_count].value     = Z_STRVAL_P(val);
                    nv_view[nv_count].value_len = Z_STRLEN_P(val);
                    nv_count++;
                } ZEND_HASH_FOREACH_END();
            }
        } ZEND_HASH_FOREACH_END();
    }

    const int status = http_response_get_status(response_obj);
    size_t body_len = 0;
    const char *body = http_response_get_body(response_obj, &body_len);

    const int submit_rc = http2_session_submit_response(
        self->session, stream->stream_id,
        status != 0 ? status : 200,
        nv_view, nv_count,
        body, body_len);

    if (nv_heap != NULL) { efree(nv_heap); }

    if (submit_rc != 0)  { return false; }

    /* Trailers. Must be queued BEFORE the drain loop so
     * the data_provider sees has_trailers=true on the final DATA
     * slice and emits NO_END_STREAM instead of END_STREAM. */
    HashTable *trailers = http_response_get_trailers(response_obj);

    if (trailers != NULL && zend_hash_num_elements(trailers) > 0) {
        http2_header_view_t tr_scratch[HTTP2_NV_SCRATCH];
        http2_header_view_t *tr_view = tr_scratch;
        http2_header_view_t *tr_heap = NULL;
        const size_t tr_count = zend_hash_num_elements(trailers);

        if (tr_count > HTTP2_NV_SCRATCH) {
            tr_heap = emalloc(tr_count * sizeof(*tr_heap));
            tr_view = tr_heap;
        }

        size_t ti = 0;
        zend_string *tn;
        zval        *tv;
        ZEND_HASH_FOREACH_STR_KEY_VAL(trailers, tn, tv) {
            if (tn == NULL || Z_TYPE_P(tv) != IS_STRING) { continue; }
            tr_view[ti].name      = ZSTR_VAL(tn);
            tr_view[ti].name_len  = ZSTR_LEN(tn);
            tr_view[ti].value     = Z_STRVAL_P(tv);
            tr_view[ti].value_len = Z_STRLEN_P(tv);
            ti++;
        } ZEND_HASH_FOREACH_END();

        (void)http2_session_submit_trailer(self->session, stream->stream_id,
                                           tr_view, ti);

        if (tr_heap != NULL) { efree(tr_heap); }
    }

    /* GOAWAY after response+trailers so it bundles with the final DATA
     * in one writev. drain_submitted guards multi-stream double-GOAWAY. */
    if (!conn->drain_submitted
        && http_server_should_drain_now(conn->server, conn, zend_hrtime())) {
        (void)http2_session_terminate(self->session, 0 /* NGHTTP2_NO_ERROR */);
        conn->drain_submitted = true;
        http_server_on_h2_goaway_sent(conn->counters);
    }

    http2_session_emit(self->session);
    return true;
}

/* -------------------------------------------------------------------------
 * Streaming response — vtable exported for HttpResponse::send().
 * All three ops take the http2_stream_t* ctx that dispatch stashed
 * into the PHP response object. They rely on the stream's
 * http2_session + owning connection staying alive for as long as the
 * response zval is — enforced by the coroutine refcount on the stream.
 * ------------------------------------------------------------------------- */

/* Commit status + headers using the streaming submit path. Called
 * lazily on the first h2_stream_append_chunk — buffered handlers
 * never touch this. Returns false on submit error (bad header set,
 * session gone). */
static bool h2_commit_streaming_headers(http_connection_t *conn,
                                        http2_stream_t *stream)
{
    if (conn == NULL || conn->strategy == NULL || stream == NULL
        || Z_ISUNDEF(stream->response_zv)) {
        return false;
    }

    zend_object *response_obj = Z_OBJ(stream->response_zv);
    http2_strategy_t *self = (http2_strategy_t *)conn->strategy;

    if (self->session == NULL) { return false; }

    /* Flatten headers — identical two-pass to commit_stream_response,
     * just bundled with submit_response_streaming at the end. */
    HashTable *headers = http_response_get_headers(response_obj);

    size_t total_values = 0;

    if (headers != NULL) {
        zend_string *name;
        zval        *values;
        ZEND_HASH_FOREACH_STR_KEY_VAL(headers, name, values) {
            if (name == NULL)                                            continue;

            if (!http_response_header_allowed_h2h3(ZSTR_VAL(name), ZSTR_LEN(name))) continue;

            if (EXPECTED(Z_TYPE_P(values) == IS_STRING)) {
                total_values++;
            } else if (Z_TYPE_P(values) == IS_ARRAY) {
                zval *val;
                ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(values), val) {
                    if (Z_TYPE_P(val) == IS_STRING) { total_values++; }
                } ZEND_HASH_FOREACH_END();
            }
        } ZEND_HASH_FOREACH_END();
    }

    http2_header_view_t scratch[HTTP2_NV_SCRATCH];
    http2_header_view_t *nv_view = scratch;
    http2_header_view_t *nv_heap = NULL;

    if (total_values > HTTP2_NV_SCRATCH) {
        nv_heap = emalloc(total_values * sizeof(*nv_heap));
        nv_view = nv_heap;
    }

    size_t nv_count = 0;

    if (headers != NULL) {
        zend_string *name;
        zval        *values;
        ZEND_HASH_FOREACH_STR_KEY_VAL(headers, name, values) {
            if (name == NULL)                                            continue;

            if (!http_response_header_allowed_h2h3(ZSTR_VAL(name), ZSTR_LEN(name))) continue;

            if (EXPECTED(Z_TYPE_P(values) == IS_STRING)) {
                nv_view[nv_count].name      = ZSTR_VAL(name);
                nv_view[nv_count].name_len  = ZSTR_LEN(name);
                nv_view[nv_count].value     = Z_STRVAL_P(values);
                nv_view[nv_count].value_len = Z_STRLEN_P(values);
                nv_count++;
            } else if (Z_TYPE_P(values) == IS_ARRAY) {
                zval *val;
                ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(values), val) {
                    if (Z_TYPE_P(val) != IS_STRING) { continue; }
                    nv_view[nv_count].name      = ZSTR_VAL(name);
                    nv_view[nv_count].name_len  = ZSTR_LEN(name);
                    nv_view[nv_count].value     = Z_STRVAL_P(val);
                    nv_view[nv_count].value_len = Z_STRLEN_P(val);
                    nv_count++;
                } ZEND_HASH_FOREACH_END();
            }
        } ZEND_HASH_FOREACH_END();
    }

    const int status = http_response_get_status(response_obj);
    const int rc = http2_session_submit_response_streaming(
        self->session, stream->stream_id,
        status != 0 ? status : 200,
        nv_view, nv_count);

    if (nv_heap != NULL) { efree(nv_heap); }
    return rc == 0;
}

/* Release body ref: zend_object (buffered) or zend_string (chunk), via GC_TYPE. */
static void h2_emit_ref_release(zend_refcounted *ref)
{
    if (GC_TYPE(ref) == IS_STRING) {
        zend_string_release((zend_string *)ref);
    } else {
        zend_object *obj = (zend_object *)ref;
        OBJ_RELEASE(obj);
    }
}

/* NOT for the h2c success path — there ownership moves into the writev ctx. */
static void h2_emit_state_cleanup(struct http2_emit_state *st)
{
    for (unsigned i = 0; i < st->body_refs_count; i++) {
        h2_emit_ref_release(st->body_refs[i]);
    }

    if (st->body_refs != NULL) { efree(st->body_refs); }

    if (st->records != NULL) { efree(st->records); }

    if (st->emit_buf_on_heap) { efree(st->emit_buf); }
}

#ifdef HAVE_OPENSSL
static bool h2_tls_write_chunk(http_connection_t *conn,
                               const char *ptr, const size_t len)
{
    size_t off = 0;

    while (off < len) {
        size_t written = 0;
        const tls_io_result_t wrc = tls_write_plaintext(
            conn->tls, ptr + off, len - off, &written);

        off += written;

        if (written > 0) {
            http_server_on_tls_io(conn->counters, 0, written, 0, 0);
        }

        if (wrc != TLS_IO_OK) {
            return false;
        }
    }

    return true;
}

/* TLS emit: body records >= H2_TLS_RECORD_PAYLOAD_MAX go zero-copy;
 * smaller records pack into one TLS record via stage[]. */
static void h2_emit_flush_tls(http_connection_t *conn,
                              http2_session_t *session,
                              struct http2_emit_state *st)
{
    /* Gate on cipher_inflight; tls_cipher_completion re-enters us. */
    (void)tls_drain(conn);
    if (conn->tls_cipher_inflight > 0) {
        session->emit_state = NULL;
        return;
    }

    const int rc = nghttp2_session_send(session->ng);
    session->emit_state = NULL;

    bool ok = (rc == 0);

    if (ok && st->records == NULL) {
        if (st->emit_buf_len > 0) {
            ok = h2_tls_write_chunk(conn, st->emit_buf, st->emit_buf_len);
        }

        h2_emit_state_cleanup(st);

        if (!ok) { conn->tls_write_error = true; }

        (void)tls_drain(conn);
        return;
    }

    char   stage[H2_TLS_RECORD_PAYLOAD_MAX];
    size_t stage_len = 0;

    for (unsigned i = 0; ok && i < st->records_count; i++) {
        const http2_emit_record_t *rec = &st->records[i];
        const char *ptr = rec->is_body
                            ? rec->body.ptr
                            : st->emit_buf + rec->buf.offset;
        const size_t len = rec->is_body ? rec->body.len : rec->buf.len;

        if (len >= H2_TLS_RECORD_PAYLOAD_MAX) {
            if (stage_len > 0) {
                ok = h2_tls_write_chunk(conn, stage, stage_len);
                stage_len = 0;
                if (!ok) { break; }
            }

            ok = h2_tls_write_chunk(conn, ptr, len);
            continue;
        }

        if (stage_len + len > H2_TLS_RECORD_PAYLOAD_MAX) {
            ok = h2_tls_write_chunk(conn, stage, stage_len);
            stage_len = 0;
            if (!ok) { break; }
        }

        memcpy(stage + stage_len, ptr, len);
        stage_len += len;
    }

    if (ok && stage_len > 0) {
        ok = h2_tls_write_chunk(conn, stage, stage_len);
    }

    h2_emit_state_cleanup(st);

    if (!ok) {
        conn->tls_write_error = true;
    }

    (void)tls_drain(conn);
}
#endif

/* h2c emit: flatten records[] (emit_buf slices + NO_COPY body) into an
 * iov[] shipped through one zero-copy writev_ex. */
static void h2_emit_flush_h2c(http_connection_t *conn,
                              http2_session_t *session,
                              struct http2_emit_state *st)
{
    const int rc = nghttp2_session_send(session->ng);
    session->emit_state = NULL;

    if (UNEXPECTED(rc != 0) || st->emit_buf_len == 0) {
        h2_emit_state_cleanup(st);
        return;
    }

    /* iov pointers outlive this frame — heap-promote. */
    if (!st->emit_buf_on_heap) {
        char *heap = emalloc(st->emit_buf_len);
        memcpy(heap, st->emit_buf, st->emit_buf_len);
        st->emit_buf = heap;
        st->emit_buf_on_heap = true;
    }

    /* HEADERS-only pass: synthesize one iov over emit_buf. */
    const unsigned niov = st->records != NULL ? st->records_count : 1u;
    zend_async_buf_t *iov = emalloc(niov * sizeof(*iov));

    if (st->records == NULL) {
        iov[0].base = st->emit_buf;
        iov[0].len  = st->emit_buf_len;
    } else {
        for (unsigned i = 0; i < st->records_count; i++) {
            const http2_emit_record_t *rec = &st->records[i];
            if (rec->is_body) {
                iov[i].base = (char *)rec->body.ptr;
                iov[i].len  = rec->body.len;
            } else {
                iov[i].base = st->emit_buf + rec->buf.offset;
                iov[i].len  = rec->buf.len;
            }
        }
    }

    h2_writev_ctx_t *ctx = emalloc(sizeof(*ctx));
    ctx->emit_buf      = st->emit_buf;
    ctx->iov           = iov;
    ctx->body_refs     = st->body_refs;
    ctx->body_refs_cnt = st->body_refs_count;

    if (st->records != NULL) { efree(st->records); }

    (void)http_connection_send_batched_writev(conn, iov, niov,
                                              h2c_writev_free_cb, ctx);
}

/* zend_bailout firewall: emalloc inside emit can longjmp out of scheduler
 * context (writev completion → notify_emit) and crash finalize. */
static void http2_emit_log_bailout(const http_connection_t *conn)
{
    const zend_string *last_err = PG(last_error_message);
    const char *cause = (last_err != NULL) ? ZSTR_VAL(last_err) : "(no PG message)";

    fprintf(stderr,
            "[true-async-server] zend_bailout in h2 emit: conn=%p — %s\n",
            (const void *)conn, cause);
    fflush(stderr);
}

void http2_session_emit(http2_session_t *session)
{
    http_connection_t *conn = http2_session_get_conn(session);

    if (conn->write_timed_out) {
        return;
    }

    char stack_buf[32768];
    struct http2_emit_state st = {
        .emit_buf     = stack_buf,
        .emit_buf_cap = sizeof(stack_buf),
    };

    volatile bool bailout = false;
    zend_try
    {
#ifdef HAVE_OPENSSL
        if (conn->tls != NULL) {
            /* byte_cap = ring/2 keeps plaintext under cipher_bio after one
             * chunk overshoot (16 KiB + framehd). */
            st.byte_cap   = 32 * 1024;
            session->emit_state = &st;
            h2_emit_flush_tls(conn, session, &st);
        } else
#endif
        /* Skip if writev in flight; completion re-drives via notify_emit. */
        if (!conn->out_in_flight) {
            session->emit_state = &st;
            h2_emit_flush_h2c(conn, session, &st);
        }
    }

    zend_catch
    {
        bailout = true;
    }

    zend_end_try();

    if (UNEXPECTED(bailout)) {
        session->emit_state = NULL;
        h2_emit_state_cleanup(&st);
        conn->write_timed_out = true;

        if (conn->io != NULL) {
            ZEND_ASYNC_IO_CLOSE(conn->io);
        }

        http2_emit_log_bailout(conn);
    }
}

static void h2c_writev_free_cb(void *user_data, zend_async_io_t *io)
{
    (void)io;
    h2_writev_ctx_t *ctx = (h2_writev_ctx_t *)user_data;

    for (unsigned i = 0; i < ctx->body_refs_cnt; i++) {
        h2_emit_ref_release(ctx->body_refs[i]);
    }

    if (ctx->body_refs != NULL) { efree(ctx->body_refs); }

    if (ctx->iov != NULL)       { efree(ctx->iov); }

    if (ctx->emit_buf != NULL)  { efree(ctx->emit_buf); }

    efree(ctx);
}

/* Park on stream->write_event (woken by WINDOW_UPDATE / ring-slot drain)
 * with write_timeout fallback. false on timeout/cancel (exception set). */
static bool h2_wait_for_drain_event(http2_stream_t *stream,
                                    http_connection_t *conn)
{
    zend_coroutine_t *co = ZEND_ASYNC_CURRENT_COROUTINE;

    if (co == NULL || ZEND_ASYNC_IS_SCHEDULER_CONTEXT) {
        return false;
    }

    if (stream->write_event == NULL) {
        stream->write_event = ZEND_ASYNC_NEW_TRIGGER_EVENT();

        if (stream->write_event == NULL) { return false; }
    }

    zend_async_event_t *wake_ev =
        &((zend_async_trigger_event_t *)stream->write_event)->base;

    if (ZEND_ASYNC_WAKER_NEW(co) == NULL) { return false; }

    zend_async_resume_when(co, wake_ev, false,
                           zend_async_waker_callback_resolve, NULL);

    if (conn != NULL && conn->write_timeout_ms > 0) {
        zend_async_event_t *timer =
            &ZEND_ASYNC_NEW_TIMER_EVENT(
                (zend_ulong)conn->write_timeout_ms, false)->base;
        zend_async_resume_when(co, timer, true,
                               zend_async_waker_callback_timeout, NULL);
    }

    ZEND_ASYNC_SUSPEND();
    zend_async_waker_clean(co);

    if (EG(exception) != NULL) {
        return false;
    }

    return true;
}

/* Per-stream ring slot count. Dual-bounded backpressure: slot cap OR
 * stream_write_buffer_bytes high-water mark. */
#define H2_CHUNK_RING_SLOTS 8

static int h2_stream_append_chunk(void *ctx, zend_string *chunk)
{
    http2_stream_t *stream = (http2_stream_t *)ctx;
    http_connection_t *conn = http2_session_get_conn(stream->session);

    if (conn->write_timed_out) {
        zend_string_release(chunk);
        return HTTP_STREAM_APPEND_STREAM_DEAD;
    }

    if (stream->chunk_queue == NULL) {
        stream->chunk_queue_cap   = H2_CHUNK_RING_SLOTS;
        stream->chunk_queue       = ecalloc(stream->chunk_queue_cap,
                                            sizeof(zend_string *));
        stream->chunk_queue_head  = 0;
        stream->chunk_queue_tail  = 0;
        stream->chunk_queue_bytes = 0;
        stream->chunk_read_offset = 0;

        if (!h2_commit_streaming_headers(conn, stream)) {
            zend_string_release(chunk);
            return HTTP_STREAM_APPEND_STREAM_DEAD;
        }

        http_server_on_streaming_response_started(conn->counters);
    }

    http_server_counters_t *counters = conn->counters;

    /* 0 disables byte cap; slot cap still bounds the ring. */
    const uint32_t max_bytes = conn->server != NULL
                             ? http_server_get_stream_write_buffer_bytes(conn->server)
                             : 0;

    /* Backpressure: park until a slot frees AND queued bytes drop below
     * max_bytes. Ring is compacted to index 0 so tail can advance. */
    for (;;) {
        if (stream->chunk_queue_head > 0) {
            const size_t live =
                stream->chunk_queue_tail - stream->chunk_queue_head;

            if (live > 0) {
                memmove(stream->chunk_queue,
                        stream->chunk_queue + stream->chunk_queue_head,
                        live * sizeof(zend_string *));
            }

            stream->chunk_queue_head = 0;
            stream->chunk_queue_tail = live;
        }

        if (stream->chunk_queue_tail < stream->chunk_queue_cap
            && (max_bytes == 0 || stream->chunk_queue_bytes < max_bytes)) {
            break;
        }

        http_server_on_stream_backpressure(counters);

        if (!h2_wait_for_drain_event(stream, conn)) {
            zend_string_release(chunk);
            return HTTP_STREAM_APPEND_STREAM_DEAD;
        }

        http2_session_emit(stream->session);
    }

    stream->chunk_queue[stream->chunk_queue_tail++] = chunk;
    stream->chunk_queue_bytes += ZSTR_LEN(chunk);

    http_server_on_stream_send(counters, ZSTR_LEN(chunk));

    /* Resume DEFERRED data provider + emit. */
    (void)http2_session_resume_stream_data(stream->session, stream->stream_id);
    http2_session_emit(stream->session);

    return HTTP_STREAM_APPEND_OK;
}

static void h2_stream_mark_ended(void *ctx)
{
    http2_stream_t *stream = (http2_stream_t *)ctx;

    if (stream->streaming_ended) {
        return;
    }

    stream->streaming_ended = true;

    /* Peer-RST removed nghttp2's stream state; resume + mem_send would
     * walk a stale data_provider and crash (phpt 092). */
    if (stream->peer_closed) {
        return;
    }

    (void)http2_session_resume_stream_data(stream->session, stream->stream_id);

    http2_session_emit(stream->session);
}

static zend_async_event_t *h2_stream_get_wait_event(void *ctx)
{
    http2_stream_t *stream = (http2_stream_t *)ctx;

    if (stream->write_event == NULL) {
        stream->write_event = ZEND_ASYNC_NEW_TRIGGER_EVENT();
    }

    return stream->write_event != NULL
               ? &((zend_async_trigger_event_t *)stream->write_event)->base
               : NULL;
}

/* sendable() backing: true if append_chunk would proceed without suspend. */
static bool h2_stream_sendable(void *ctx)
{
    http2_stream_t *stream = (http2_stream_t *)ctx;

    if (stream->chunk_queue == NULL) {
        return true;   /* not started — first send() always proceeds */
    }

    if (stream->chunk_queue_tail - stream->chunk_queue_head
            >= stream->chunk_queue_cap) {
        return false;  /* all slots live */
    }

    const http_connection_t *conn = http2_session_get_conn(stream->session);
    const uint32_t max_bytes = conn->server != NULL
                             ? http_server_get_stream_write_buffer_bytes(conn->server)
                             : 0;

    return max_bytes == 0 || stream->chunk_queue_bytes < max_bytes;
}

const http_response_stream_ops_t h2_stream_ops = {
    .append_chunk        = h2_stream_append_chunk,
    .sendable            = h2_stream_sendable,
    .mark_ended          = h2_stream_mark_ended,
    .get_wait_event      = h2_stream_get_wait_event,
    .send_static_response = h2_stream_send_static_response,
};

/* External entry for tls_cipher_completion. No-op on non-h2. */
void http2_conn_notify_emit(http_connection_t *conn)
{
    if (conn == NULL || conn->strategy == NULL ||
        conn->protocol_type != HTTP_PROTOCOL_HTTP2) {
        return;
    }

    const http2_strategy_t *self = (const http2_strategy_t *)conn->strategy;

    if (self->session != NULL) {
        http2_session_emit(self->session);
    }
}

static void http2_strategy_send_response(http_connection_t *conn, void *response)
{
    (void)conn;
    (void)response;
    /* The HTTP/1 dispose path still owns the format+send flow and the
     * HTTP/2 side routes through http2_strategy_commit_response
     * directly (called from dispose on protocol==HTTP2). */
}

static void http2_strategy_reset(http_connection_t *conn)
{
    (void)conn;
    /* No-op for HTTP/2: per-request reset happens at the stream level,
     * not at the connection level. Streams are created/destroyed inside
     * the nghttp2 callback table. */
}

static void http2_strategy_cleanup(http_connection_t *conn)
{
    if (conn == NULL || conn->strategy == NULL) {
        return;
    }

    http2_strategy_t *self = (http2_strategy_t *)conn->strategy;

    if (self->session != NULL) {
        http2_session_free(self->session);
        self->session = NULL;
    }
}

http_protocol_strategy_t *http_protocol_strategy_http2_create(void)
{
    http2_strategy_t *self = ecalloc(1, sizeof(*self));

    self->base.protocol_type    = HTTP_PROTOCOL_HTTP2;
    self->base.name             = "HTTP/2";
    self->base.on_request_ready = NULL;   /* wired by the connection layer */
    self->base.feed             = http2_feed;
    self->base.send_response    = http2_strategy_send_response;
    self->base.reset            = http2_strategy_reset;
    self->base.cleanup          = http2_strategy_cleanup;
    self->session               = NULL;

    return &self->base;
}

