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
#include "http2/http2_session.h"
#include "http2/http2_stream.h"
#include "http1/http_parser.h"   /* http_request_t */

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

/* Session calls this on HEADERS + END_HEADERS for each new stream.
 * Per plan §3.6 this fires regardless of END_STREAM so gRPC bidi
 * handlers start running before the body arrives.
 *
 * Spawns a per-stream handler coroutine whose extended_data is the
 * http2_stream_t*. Each stream's request_zv / response_zv live on
 * the stream, so concurrent streams can't trample each other —
 * unlike HTTP/1's one-request-per-connection generic on_request_ready
 * path. */
static void http2_strategy_dispatch(struct http_request_t *const request,
                                    const uint32_t stream_id,
                                    void *const user_data)
{
    (void)request;      /* Reachable via stream->request; we use that path. */
    http2_strategy_t *const self = (http2_strategy_t *)user_data;

    http2_stream_t *const stream = http2_session_find_stream(self->session,
                                                             stream_id);
    if (stream == NULL || self->conn == NULL || self->conn->handler == NULL) {
        return;
    }

    /* Build the PHP HttpRequest object from the parsed request. The
     * object takes ownership of the http_request_t — stream_free's
     * `request_dispatched` guard already knows not to free it again. */
    zval *const req_obj = http_request_create_from_parsed(stream->request);
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

    /* Spawn the handler coroutine. extended_data is the STREAM, not
     * the connection — that's what makes multiplex safe: N
     * coroutines hold N distinct stream pointers, each pointing at
     * its own zvals. */
    zend_coroutine_t *const co = ZEND_ASYNC_NEW_COROUTINE(self->conn->scope);
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
    stream->request->enqueue_ns = zend_hrtime();

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
    const zend_coroutine_t *const co = ZEND_ASYNC_CURRENT_COROUTINE;
    http2_stream_t *const stream = (http2_stream_t *)co->extended_data;
    if (stream == NULL || stream->session == NULL) { return; }

    http_connection_t *const conn = http2_session_get_conn(stream->session);
    if (conn == NULL || conn->handler == NULL) { return; }

    if (stream->request != NULL) {
        stream->request->start_ns = zend_hrtime();
    }

    zval params[2], retval;
    ZVAL_COPY_VALUE(&params[0], &stream->request_zv);
    ZVAL_COPY_VALUE(&params[1], &stream->response_zv);
    ZVAL_UNDEF(&retval);

    call_user_function(NULL, NULL, &conn->handler->fci.function_name,
                       &retval, 2, params);

    /* Stamp end_ns + feed backpressure sample BEFORE retval dtor so
     * destructor time on a returned object doesn't count as service
     * time. Matches the HTTP/1 handler-coroutine discipline. */
    if (stream->request != NULL) {
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

/* Guaranteed cleanup — runs on normal return, exception, or
 * cancellation (peer RST_STREAM / server stop). The stream remains
 * in nghttp2's table until on_stream_close_cb tears it down; we
 * only release the PHP-side state here. */
static void http2_handler_coroutine_dispose(zend_coroutine_t *coroutine)
{
    http2_stream_t *const stream = (http2_stream_t *)coroutine->extended_data;
    if (stream == NULL || stream->session == NULL) { return; }

    /* Break the back-pointer BEFORE anything else so a late
     * RST_STREAM handler can't re-enter ZEND_ASYNC_CANCEL on a
     * coroutine that's already tearing down. Same discipline as the
     * HTTP/1 coroutine dispose path. */
    stream->coroutine = NULL;
    if (stream->request != NULL) {
        stream->request->coroutine = NULL;
    }

    http_connection_t *const conn = http2_session_get_conn(stream->session);

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
        zend_object *const exc = coroutine->exception;

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

    /* Streaming path (send() was called): HEADERS + DATA already
     * committed via h2_stream_ops. Just make sure the stream is
     * marked ended — if the handler forgot to call end(), we do it
     * here so the data provider emits EOF. Skip the buffered-mode
     * commit entirely (re-submitting response would RST the stream). */
    const bool is_streaming = !Z_ISUNDEF(stream->response_zv)
                              && http_response_is_streaming(Z_OBJ(stream->response_zv));

    if (is_streaming) {
        if (!stream->streaming_ended) {
            h2_stream_mark_ended(stream);
        }
    } else if (conn != NULL && !Z_ISUNDEF(stream->response_zv)) {
        /* Buffered-mode commit (normal REST path). */
        (void)http2_commit_stream_response(conn, stream);
    }

    /* Drop the PHP objects, then release our refcount. If the
     * session table already released its reference (nghttp2 closed
     * the stream while we were still draining), this final release
     * triggers efree; otherwise the table-held ref keeps the stream
     * alive until on_stream_close_cb fires. */
    zval_ptr_dtor(&stream->request_zv);
    ZVAL_UNDEF(&stream->request_zv);
    zval_ptr_dtor(&stream->response_zv);
    ZVAL_UNDEF(&stream->response_zv);

    http2_stream_release(stream);

    /* Release the conn-level handler ref and finalise a deferred
     * destroy if the read-callback marked one while we ran. Stash
     * conn locally because the release may free it. */
    if (conn != NULL) {
        if (conn->handler_refcount > 0) {
            conn->handler_refcount--;
        }
        if (conn->handler_refcount == 0 && conn->destroy_pending) {
            conn->destroy_pending = false;
            http_connection_destroy(conn);
        }
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

/* Forbidden / HTTP-1-only response headers per RFC 9113 §8.2.2.
 * nghttp2 would reject them at submit time anyway; filter here so a
 * handler that sets e.g. Connection: close for HTTP/1 habit doesn't
 * kill the whole stream. */
static bool response_header_allowed(const char *const name, const size_t len)
{
    if (len == 10 && strncasecmp(name, "connection", 10) == 0) return false;
    if (len == 10 && strncasecmp(name, "keep-alive", 10) == 0) return false;
    if (len == 17 && strncasecmp(name, "transfer-encoding", 17) == 0) return false;
    if (len == 7  && strncasecmp(name, "upgrade", 7) == 0)  return false;
    if (len == 14 && strncasecmp(name, "content-length", 14) == 0) return false; /* implicit via DATA */
    return true;
}

/* Commit a response on a specific stream. Per-stream dispatch means
 * we already KNOW which stream, no scan needed.
 * Extracts status / headers / body / trailers from the PHP
 * HttpResponse, submits via http2_session_submit_response +
 * http2_session_submit_trailer, then drains HEADERS + DATA +
 * optional HEADERS(trailers) into the socket through
 * http_connection_send. */
static bool http2_commit_stream_response(http_connection_t *const conn,
                                         http2_stream_t *const stream)
{
    if (conn == NULL || conn->strategy == NULL || stream == NULL ||
        Z_ISUNDEF(stream->response_zv)) {
        return false;
    }
    zend_object *const response_obj = Z_OBJ(stream->response_zv);
    http2_strategy_t *const self = (http2_strategy_t *)conn->strategy;
    if (self->session == NULL) {
        return false;
    }

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
    HashTable *const headers = http_response_get_headers(response_obj);

    size_t total_values = 0;
    if (headers != NULL) {
        zend_string *name;
        zval        *values;
        ZEND_HASH_FOREACH_STR_KEY_VAL(headers, name, values) {
            if (name == NULL)                                            continue;
            if (!response_header_allowed(ZSTR_VAL(name), ZSTR_LEN(name))) continue;
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
            if (!response_header_allowed(ZSTR_VAL(name), ZSTR_LEN(name))) continue;
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
    const char *const body = http_response_get_body(response_obj, &body_len);

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
    HashTable *const trailers = http_response_get_trailers(response_obj);
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

    /* Graceful connection drain. Submitted AFTER response + trailers
     * are queued so the GOAWAY
     * frame bundles into the same writev batch as the final DATA —
     * client receives its full response plus the "no new streams"
     * signal in one round-trip. drain_submitted guards against
     * double-GOAWAY when multiple concurrent streams commit on the
     * same session; existing streams continue to drain naturally
     * after GOAWAY per RFC 9113 §6.8. */
    if (!conn->drain_submitted
        && http_server_should_drain_now(conn->server, conn, zend_hrtime())) {
        (void)http2_session_terminate(self->session, 0 /* NGHTTP2_NO_ERROR */);
        conn->drain_submitted = true;
        http_server_on_h2_goaway_sent(conn->counters);
    }

    /* Drain nghttp2's outbound buffer into the socket. 16 KiB chunks
     * match MAX_FRAME_SIZE so each send_raw call carries at most one
     * full frame. Loops until nghttp2 reports no more bytes pending
     * or a write fails. */
    char drain_buf[16384];
    while (http2_session_want_write(self->session)) {
        const ssize_t n = http2_session_drain(self->session,
                                              drain_buf, sizeof(drain_buf));
        if (n < 0) { return false; }
        if (n == 0) { break; }
        if (!http_connection_send(conn, drain_buf, (size_t)n)) {
            return false;
        }
    }
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
static bool h2_commit_streaming_headers(http_connection_t *const conn,
                                        http2_stream_t *const stream)
{
    if (conn == NULL || conn->strategy == NULL || stream == NULL
        || Z_ISUNDEF(stream->response_zv)) {
        return false;
    }
    zend_object *const response_obj = Z_OBJ(stream->response_zv);
    http2_strategy_t *const self = (http2_strategy_t *)conn->strategy;
    if (self->session == NULL) { return false; }

    /* Flatten headers — identical two-pass to commit_stream_response,
     * just bundled with submit_response_streaming at the end. */
    HashTable *const headers = http_response_get_headers(response_obj);

    size_t total_values = 0;
    if (headers != NULL) {
        zend_string *name;
        zval        *values;
        ZEND_HASH_FOREACH_STR_KEY_VAL(headers, name, values) {
            if (name == NULL)                                            continue;
            if (!response_header_allowed(ZSTR_VAL(name), ZSTR_LEN(name))) continue;
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
            if (!response_header_allowed(ZSTR_VAL(name), ZSTR_LEN(name))) continue;
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

/* Drive nghttp2 to push whatever it can onto the socket right now.
 * Runs from the handler coroutine context — http_connection_send is
 * safe here (proper async suspend/resume through the TrueAsync I/O
 * waker). Same 16 KiB chunk size as commit_stream_response. */
static void h2_drain_to_socket(http_connection_t *const conn,
                               http2_session_t *const session)
{
    char buf[16384];
    while (http2_session_want_write(session)) {
        const ssize_t n = http2_session_drain(session, buf, sizeof(buf));
        if (n <= 0) { break; }
        if (!http_connection_send(conn, buf, (size_t)n)) {
            break;
        }
    }
}

/* Suspend-until-drain-event helper. Called from append_chunk's tail
 * loop when flow control keeps bytes in the queue after a drain pass.
 * See the "Race-free" comment block at the call site for why this
 * doesn't deadlock.
 *
 * Two wake sources, whichever fires first wins:
 *   1. stream->write_event (trigger event) — fired from
 *      cb_on_frame_recv on WINDOW_UPDATE.
 *   2. write_timeout timer — defensive, caps wait. Maps write_timeout
 *      from the connection; 0 disables the timer (wait forever, used
 *      only in tests / bring-up).
 *
 * Returns true on event wake, false on timeout (EG(exception) set) or
 * cancellation (peer RST — also sets exception). Caller must not
 * suspend outside a coroutine — we check up front.
 *
 * Lifecycle: stream->write_event is lazy — created on first wait, kept
 * across subsequent waits, disposed in http2_stream_release. */
static bool h2_wait_for_drain_event(http2_stream_t *const stream,
                                    http_connection_t *const conn)
{
    zend_coroutine_t *const co = ZEND_ASYNC_CURRENT_COROUTINE;
    if (co == NULL || ZEND_ASYNC_IS_SCHEDULER_CONTEXT) {
        return false;
    }

    /* Lazy-create the wake event once per stream. */
    if (stream->write_event == NULL) {
        stream->write_event = ZEND_ASYNC_NEW_TRIGGER_EVENT();
        if (stream->write_event == NULL) { return false; }
    }
    zend_async_event_t *const wake_ev =
        &((zend_async_trigger_event_t *)stream->write_event)->base;

    if (ZEND_ASYNC_WAKER_NEW(co) == NULL) { return false; }

    zend_async_resume_when(co, wake_ev, false,
                           zend_async_waker_callback_resolve, NULL);

    /* Write-timeout fallback for dead / misbehaving peers. Without
     * this, a peer that stops acknowledging WINDOW_UPDATE can pin
     * the handler forever. */
    if (conn != NULL && conn->write_timeout_ms > 0) {
        zend_async_event_t *const timer =
            &ZEND_ASYNC_NEW_TIMER_EVENT(
                (zend_ulong)conn->write_timeout_ms, false)->base;
        zend_async_resume_when(co, timer, true,
                               zend_async_waker_callback_timeout, NULL);
    }

    ZEND_ASYNC_SUSPEND();
    zend_async_waker_clean(co);

    if (EG(exception) != NULL) {
        /* Timeout exception is expected for genuinely stalled clients;
         * map to "stream dead" so send() unwinds cleanly. Other
         * exceptions (peer RST propagated as cancel) likewise exit. */
        return false;
    }
    return true;
}

static int h2_stream_append_chunk(void *const ctx, zend_string *const chunk)
{
    http2_stream_t *const stream = (http2_stream_t *)ctx;
    if (stream == NULL || stream->session == NULL) {
        zend_string_release(chunk);
        return HTTP_STREAM_APPEND_STREAM_DEAD;
    }
    http_connection_t *const conn = http2_session_get_conn(stream->session);
    if (conn == NULL) {
        zend_string_release(chunk);
        return HTTP_STREAM_APPEND_STREAM_DEAD;
    }

    /* Lazy queue allocation + first-send HEADERS commit. */
    if (stream->chunk_queue == NULL) {
        stream->chunk_queue_cap  = 8;
        stream->chunk_queue      = ecalloc(stream->chunk_queue_cap,
                                           sizeof(zend_string *));
        stream->chunk_queue_head = 0;
        stream->chunk_queue_tail = 0;
        stream->chunk_queue_bytes = 0;
        stream->chunk_read_offset = 0;

        if (!h2_commit_streaming_headers(conn, stream)) {
            zend_string_release(chunk);
            return HTTP_STREAM_APPEND_STREAM_DEAD;
        }
        /* One new streaming response activated. */
        http_server_on_streaming_response_started(conn->counters);
    }
    http_server_counters_t *const counters = conn->counters;

    /* Grow ring if full. Compact first (shift head → 0) to avoid
     * unbounded growth on long-lived streams. */
    if (stream->chunk_queue_tail == stream->chunk_queue_cap) {
        if (stream->chunk_queue_head > 0) {
            const size_t live = stream->chunk_queue_tail - stream->chunk_queue_head;
            memmove(stream->chunk_queue,
                    stream->chunk_queue + stream->chunk_queue_head,
                    live * sizeof(zend_string *));
            stream->chunk_queue_head = 0;
            stream->chunk_queue_tail = live;
        }
        if (stream->chunk_queue_tail == stream->chunk_queue_cap) {
            const size_t new_cap = stream->chunk_queue_cap * 2;
            stream->chunk_queue = erealloc(stream->chunk_queue,
                                           new_cap * sizeof(zend_string *));
            stream->chunk_queue_cap = new_cap;
        }
    }

    stream->chunk_queue[stream->chunk_queue_tail++] = chunk;
    stream->chunk_queue_bytes += ZSTR_LEN(chunk);

    http_server_on_stream_send(counters, ZSTR_LEN(chunk));

    /* Kick nghttp2: the data provider was DEFERRED while the queue
     * was empty; resume_data tells it to retry now. */
    (void)http2_session_resume_stream_data(stream->session, stream->stream_id);

    /* Drive the pump — turn queued bytes into socket writes. */
    h2_drain_to_socket(conn, stream->session);

    /* Backpressure: if the drain didn't empty the queue,
     * flow control (per-stream or connection) is holding bytes back.
     * Suspend the handler coroutine on the stream's write_event; it
     * wakes when cb_on_frame_recv processes an inbound WINDOW_UPDATE
     * (in src/http2/http2_session.c). Write-timeout is the fallback
     * for dead peers.
     *
     * Race-free: we register the waker on our write_event BEFORE
     * suspending. The trigger fires only from the read-callback
     * (scheduler context), which cannot execute concurrently with
     * this coroutine under TrueAsync's single-threaded scope. So
     * "register → fire" ordering is guaranteed. */
    if (stream->chunk_queue_bytes > 0) {
        http_server_on_stream_backpressure(counters);
    }
    while (stream->chunk_queue_bytes > 0) {
        if (!h2_wait_for_drain_event(stream, conn)) {
            /* Timeout / cancel / peer gone. PHP exception set. */
            return HTTP_STREAM_APPEND_STREAM_DEAD;
        }
        h2_drain_to_socket(conn, stream->session);
    }

    return HTTP_STREAM_APPEND_OK;
}

static void h2_stream_mark_ended(void *const ctx)
{
    http2_stream_t *const stream = (http2_stream_t *)ctx;
    if (stream == NULL || stream->session == NULL || stream->streaming_ended) {
        return;
    }
    stream->streaming_ended = true;

    /* If the peer already RST'd this stream, nghttp2's internal state
     * for stream_id is gone. resume_stream_data is a no-op in that
     * case per the nghttp2 API, but the subsequent mem_send call can
     * still walk a stale data-provider pointer and dereference a NULL
     * function slot — crashes under concurrent peer-reset-mid-stream
     * loads (phpt 092). Nothing more to push; dispose unwinds. */
    if (stream->peer_closed) {
        return;
    }

    /* Wake the data provider one last time — it'll see empty queue
     * + streaming_ended and flag EOF (or NO_END_STREAM + trailers). */
    (void)http2_session_resume_stream_data(stream->session, stream->stream_id);

    http_connection_t *const conn = http2_session_get_conn(stream->session);
    if (conn != NULL) {
        h2_drain_to_socket(conn, stream->session);
    }
}

static zend_async_event_t *h2_stream_get_wait_event(void *const ctx)
{
    http2_stream_t *const stream = (http2_stream_t *)ctx;
    if (stream == NULL) { return NULL; }
    if (stream->write_event == NULL) {
        stream->write_event = ZEND_ASYNC_NEW_TRIGGER_EVENT();
    }
    return stream->write_event != NULL
               ? &((zend_async_trigger_event_t *)stream->write_event)->base
               : NULL;
}

const http_response_stream_ops_t h2_stream_ops = {
    .append_chunk   = h2_stream_append_chunk,
    .mark_ended     = h2_stream_mark_ended,
    .get_wait_event = h2_stream_get_wait_event,
};

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

