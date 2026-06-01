/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+

  HTTP/3 user-handler dispatch + per-stream coroutine lifecycle.
*/

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif


#include "http3_internal.h"               /* php.h + Zend/zend_async_API.h +
                                            * ngtcp2 + nghttp3 + openssl/ssl.h +
                                            * http3_connection.h + php_http_server.h */
#include "Zend/zend_hrtime.h"              /* enqueue_ns / start_ns / end_ns */
#include "core/http_protocol_handlers.h"   /* http_protocol_get_handler */
#include "http3_listener.h"                /* http3_listener_server_obj */
#include "http3/http3_stream.h"            /* http3_stream_t */
#include "log/trace_context.h"
#include "http_connection.h"               /* http_handler_log_bailout */
#include "http_send_file.h"                /* http_send_file_dispatch */
#include "http_response_internal.h"        /* http_response_has/take_send_file */
#include "static/static_handler.h"         /* http_static_try_serve / count */

/* Defined in src/http_request.c. Declared here because the public
 * php_http_server.h header doesn't expose it (it lives in the C boundary
 * between the parser and the PHP HttpRequest object). */
extern zval *http_request_create_from_parsed(http_request_t *req);

/* http3_listener_server_obj already declared in http3_listener.h. */

/* Forward decls for the entry / dispose pair, mutually referenced
 * through the coroutine vtable. */
static void h3_handler_coroutine_entry(void);
static void h3_handler_coroutine_dispose(zend_coroutine_t *coroutine);
static void h3_dispose_tail(http3_connection_t *c, http3_stream_t *s);

/* Lifecycle trace (temporary, gated by H3_TRACE). Microsecond timestamp +
 * stream id at each stage, to see where a request's time goes under load. */
static inline int h3_trace_on(void)
{
    static int v = -1;
    if (v < 0) { v = getenv("H3_TRACE") != NULL ? 1 : 0; }
    return v;
}
#define H3T(sid, ev) do { if (h3_trace_on()) \
    fprintf(stderr, "[h3t] %llu sid=%lld %s\n", \
        (unsigned long long)(zend_hrtime() / 1000ULL), (long long)(sid), (ev)); \
    } while (0)

/* === Static-handler dispatch callbacks ============================
 *
 * The protocol-agnostic static FSM (src/static/http_static.c) calls
 * back through these on the hard-zero async path. `user` is the
 * http3_stream_t. Mirrors h2_static_dispatch_cbs in http2_strategy.c —
 * H3 has no conn-level handler_refcount, so on_armed pins only the
 * stream (so the deferred on_done still finds a live stream +
 * response_zv to tail-dispose) plus the in-flight counter. */
static void h3_static_on_hard_zero_armed(void *user)
{
    http3_stream_t *const s = (http3_stream_t *)user;
    http3_connection_t *const c = s->conn;

    if (c != NULL) {
        http_server_on_request_dispatch(c->counters);
    }

    s->refcount++;
}

static void h3_static_on_static_done(void *user, const int status)
{
    (void)status;
    http3_stream_t *const s = (http3_stream_t *)user;
    http3_connection_t *const c = s->conn;

    if (c != NULL) {
        http_server_on_request_dispose(c->counters);
    }

    h3_dispose_tail(c, s);
}

/* H3 multiplexes on one QUIC connection; Connection/Keep-Alive are
 * filtered out of every response. Always keep-alive. */
static bool h3_static_keep_alive(void *user)
{
    (void)user;
    return true;
}

static const http_static_dispatch_cbs_t h3_static_dispatch_cbs = {
    .on_armed       = h3_static_on_hard_zero_armed,
    .on_done        = h3_static_on_static_done,
    .on_passthrough = NULL,
    .keep_alive     = h3_static_keep_alive,
};

/* Full user-handler dispatch.
 *
 * Called from h3_end_stream_cb once the request is fully assembled.
 * Builds the per-stream PHP zvals (HttpRequest, HttpResponse), spawns
 * a TrueAsync coroutine in a per-request scope (child of the server
 * scope) so the handler can suspend (await), and hands off. The
 * coroutine's dispose path serialises the
 * response back through nghttp3 and triggers a drain — that is what
 * makes the response actually leave the box.
 *
 * If the server has no handler registered (defensive — addHttpHandler
 * is normally a hard requirement of HttpServer::start) we fall back to
 * a 500 so the peer never sees an indefinite half-open stream. */
void http3_stream_dispatch(http3_connection_t *c, http3_stream_t *s)
{
    if (c == NULL || s == NULL || s->dispatched || s->request == NULL) {
        return;
    }

    http_server_object *server =
        (http_server_object *)http3_listener_server_obj(c->listener);

    if (server == NULL) {
        return;          /* unit-test path — no PHP context to dispatch into */
    }

    HashTable *handlers = http_server_get_protocol_handlers(server);
    zend_async_scope_t *scope = http_server_get_scope(server);

    /* Pick the user handler. addHttpHandler stores it as HTTP1; H2 has
     * its own slot. Try H1 first, then HTTP2 as a fallback so a server
     * registered only via addHttp2Handler still services H3. Symmetric
     * to how H2 strategy resolves it. */
    zend_fcall_t *fcall = http_protocol_get_handler(handlers, HTTP_PROTOCOL_HTTP1);

    if (fcall == NULL) {
        fcall = http_protocol_get_handler(handlers, HTTP_PROTOCOL_HTTP2);
    }

    /* Static-only deployments register a mount but no PHP handler — the
     * static gate below claims the request before any handler is needed.
     * Mirrors http2_strategy.c. */
    const bool has_static_mount = http_static_handler_count(server) > 0;

    if ((fcall == NULL && !has_static_mount) || scope == NULL) {
        return;
    }

    /* Mark dispatched + addref — guards against post-dispatch UAF. The
     * PHP HttpRequest object that http_request_create_from_parsed
     * spawns owns one ref; the stream keeps a second ref so
     * h3_finalize_request_body / h3_recv_data_cb can keep writing into
     * s->request->body even if the handler returns and releases the
     * HttpRequest before the body finishes streaming. Both refs drop
     * independently; the request is freed on the last release. */
    s->dispatched = true;
    s->conn = c;

    if (http_server_view(server)->telemetry_enabled) {
        http_request_parse_trace_context(s->request);
    }

    http_request_addref(s->request);

    zval *req_obj = http_request_create_from_parsed(s->request);

    if (req_obj == NULL) {
        /* PHP-side ownership transfer failed — release the addref. */
        http_request_destroy(s->request);
        s->dispatched = false;
        return;
    }

    ZVAL_COPY_VALUE(&s->request_zv, req_obj);
    efree(req_obj);

    object_init_ex(&s->response_zv, http_response_ce);
    http_response_set_protocol_version(Z_OBJ(s->response_zv), "3.0");
    /* Wire the streaming vtable so HttpResponse::send() in the
     * handler enqueues into our chunk_queue. setBody/end (REST) handlers
     * never touch this; they go through the buffered submit_response in
     * dispose. */
    http_response_install_stream_ops(Z_OBJ(s->response_zv),
                                     &h3_stream_ops, s);

#ifdef HAVE_HTTP_COMPRESSION
    /* Attach compression state (issue #8). Server pointer comes from
     * the listener — same pattern that http3_handler_coroutine uses
     * for the request-sample bookkeeping. */
    {
        extern void http_compression_attach(zend_object *,
            http_request_t *, http_server_config_t *);
        extern void http_response_set_default_json_flags(zend_object *, uint32_t);
        http_server_object *srv =
            (http_server_object *)http3_listener_server_obj(c->listener);
        http_server_config_t *cfg = http_server_get_config(srv);

        if (cfg != NULL) {
            http_compression_attach(Z_OBJ(s->response_zv),
                                    s->request, cfg);
            http_response_set_default_json_flags(
                Z_OBJ(s->response_zv), cfg->json_encode_flags);
        }
    }
#endif

    /* Static-handler dispatch (issue #60). Same policy as the H1/H2
     * sites:
     *   HARD_ZERO   — FSM owns the request; on_armed pinned the stream.
     *                 Return without spawning a coroutine; on_static_done
     *                 runs the dispose tail when the pump finishes.
     *   HANDLED     — response populated synchronously (inline small file
     *                 or 4xx). Set skip_handler so the coroutine entry
     *                 skips the user handler; dispose still commits.
     *   PASSTHROUGH — no mount matched; fall through to the handler. */
    if (UNEXPECTED(has_static_mount)) {
        const http_static_result_t static_rc =
            http_static_try_serve(server, s->request, Z_OBJ(s->response_zv),
                                  c->counters, &h3_static_dispatch_cbs, s);

        if (static_rc == HTTP_STATIC_HARD_ZERO) {
            return;
        }

        if (static_rc == HTTP_STATIC_HANDLED) {
            s->skip_handler = true;
        }
    }

    /* No PHP handler and the static path didn't claim the request:
     * synthesise a 404 so the dispose-side commit sends one. Otherwise a
     * static-only deployment would spawn a coroutine whose handler==NULL
     * guard returns silently and the stream would hang. Mirrors H1/H2. */
    if (fcall == NULL && !s->skip_handler) {
        http_response_static_set_status(Z_OBJ(s->response_zv), 404);
        http_response_static_set_header(Z_OBJ(s->response_zv),
            "content-type", 12, "text/plain; charset=utf-8", 25);
        zend_string *msg = zend_string_init("Not Found", 9, 0);
        http_response_static_set_body_str(Z_OBJ(s->response_zv), msg);
        zend_string_release(msg);
        s->skip_handler = true;
    }

    /* Spawn the per-stream handler coroutine. extended_data is the
     * STREAM (not the connection) — that's how N concurrent streams on
     * the same QUIC connection get N independent (request, response)
     * zval pairs. */
    /* Per-request (per-stream) scope + handler coroutine. See
     * http_request_handler_coroutine_new — each multiplexed stream gets
     * its own request_context() subtree, isolated from sibling streams. */
    zend_coroutine_t *co = http_request_handler_coroutine_new(
        scope, h3_handler_coroutine_entry, s, h3_handler_coroutine_dispose);

    if (co == NULL) {
        zval_ptr_dtor(&s->request_zv);  ZVAL_UNDEF(&s->request_zv);
        zval_ptr_dtor(&s->response_zv); ZVAL_UNDEF(&s->response_zv);
        s->dispatched = false;
        return;
    }

    s->coroutine = co;
    s->refcount++;

    /* Bracket on the server's in-flight counter — admission / CoDel
     * see H3 load at the right granularity. Paired with on_request_dispose
     * in h3_handler_coroutine_dispose. */
    http_server_on_request_dispatch(s->conn->counters);

    s->request->coroutine   = co;

    if (http_server_sample_stamps_enabled(s->conn->view)) {
        s->request->enqueue_ns  = zend_hrtime();
    }

    H3T(s->stream_id, "1.dispatch_enqueue");
    ZEND_ASYNC_ENQUEUE_COROUTINE(co);
}

static void h3_handler_coroutine_entry(void)
{
    const zend_coroutine_t *co = ZEND_ASYNC_CURRENT_COROUTINE;
    http3_stream_t *s = (http3_stream_t *)co->extended_data;

    if (s == NULL || s->conn == NULL) return;

    /* Static-handler HANDLED path: response_zv already carries the
     * synchronous body (inline small file or 4xx). Skip the user handler;
     * dispose runs the buffered commit. Mirrors http2_handler_coroutine_entry. */
    if (s->skip_handler) {
        http_server_count_request(s->conn->counters);
        return;
    }

    H3T(s->stream_id, "2.coro_entry");

    http_server_object *server =
        (http_server_object *)http3_listener_server_obj(s->conn->listener);
    const bool stamps = http_server_sample_stamps_enabled(s->conn->view);

    if (s->request != NULL && stamps) {
        s->request->start_ns = zend_hrtime();
    }

    HashTable *handlers = http_server_get_protocol_handlers(server);
    zend_fcall_t *fcall = http_protocol_get_handler(handlers, HTTP_PROTOCOL_HTTP1);

    if (fcall == NULL) {
        fcall = http_protocol_get_handler(handlers, HTTP_PROTOCOL_HTTP2);
    }

    if (fcall == NULL) return;

#ifdef HAVE_HTTP_COMPRESSION
    /* Inbound Content-Encoding decode (issue #8). Same shape as the
     * H1/H2 handler entries. */
    if (s->request != NULL) {
        extern int http_compression_decode_request_body(
            http_request_t *, http_server_config_t *);
        extern void http_response_set_error(zend_object *, int, const char *);
        http_server_config_t *cfg = http_server_get_config(server);
        int dec = http_compression_decode_request_body(s->request, cfg);

        if (dec != 0) {
            http_response_set_error(Z_OBJ(s->response_zv), dec,
                dec == 415 ? "Unsupported Content-Encoding" :
                dec == 413 ? "Payload Too Large after decompression" :
                             "Malformed compressed request body");
            http_server_count_request(s->conn->counters);

            if (s->request != NULL && stamps) s->request->end_ns = zend_hrtime();
            return;
        }
    }
#endif

    zval params[2], retval;
    ZVAL_COPY_VALUE(&params[0], &s->request_zv);
    ZVAL_COPY_VALUE(&params[1], &s->response_zv);
    ZVAL_UNDEF(&retval);

    zend_fcall_info fci = {
        .size           = sizeof(zend_fcall_info),
        .function_name  = fcall->fci.function_name,
        .retval         = &retval,
        .params         = params,
        .object         = NULL,
        .param_count    = 2,
        .named_params   = NULL,
    };
    /* Bailout firewall — see http_handler_log_bailout in
     * src/core/http_connection.c. */
    volatile bool bailout = false;
    zend_try
    {
        zend_call_function(&fci, &fcall->fci_cache);
    }

    zend_catch
    {
        bailout = true;
    }

    zend_end_try();

    if (UNEXPECTED(bailout)) {
        H3T(s->stream_id, "3b.handler_BAILOUT");
        const char *m = (s->request && s->request->method)
                            ? ZSTR_VAL(s->request->method) : "?";
        const char *u = (s->request && s->request->uri)
                            ? ZSTR_VAL(s->request->uri) : "?";
        http_handler_log_bailout("h3", co, m, u);
        return;
    }

    H3T(s->stream_id, "3.handler_returned");

    /* Stamp end_ns + feed backpressure sample BEFORE retval dtor so
     * destructor time on a returned object doesn't get counted as
     * service time. Same discipline as H1/H2 handler entries. Stamps
     * and the sample call are gated on sample_stamps_enabled;
     * total_requests is still bumped. */
    http_server_count_request(s->conn->counters);

    if (s->request != NULL && server != NULL && stamps) {
        s->request->end_ns = zend_hrtime();
        http_server_on_request_sample(
            server,
            s->request->start_ns - s->request->enqueue_ns,
            s->request->end_ns   - s->request->start_ns,
            s->request->end_ns);
    }

    zval_ptr_dtor(&retval);
}

/* Shared dispose tail: drop the per-stream zvals, run the graceful-drain
 * (GOAWAY) check, push queued output, and release the coroutine's stream
 * ref. Runs at the end of the normal dispose, and — deferred — from the
 * sendFile pump's on_done once the file has finished streaming. */
static void h3_dispose_tail(http3_connection_t *c, http3_stream_t *s)
{
    if (!Z_ISUNDEF(s->request_zv)) {
        zval_ptr_dtor(&s->request_zv);
        ZVAL_UNDEF(&s->request_zv);
    }

    if (!Z_ISUNDEF(s->response_zv)) {
        zval_ptr_dtor(&s->response_zv);
        ZVAL_UNDEF(&s->response_zv);
    }

    /* Graceful drain check at the response-commit point — matches H1/H2.
     * Proactive age / reactive epoch → HTTP/3 GOAWAY via nghttp3_conn_shutdown
     * so new streams are refused while the in-flight one finishes. */
    if (c != NULL && !c->closed && !c->drain_submitted
        && c->nghttp3_conn != NULL) {
        http_server_object *srv =
            (http_server_object *)http3_listener_server_obj(c->listener);
        const http_server_drain_eval_t r = http_server_drain_evaluate(srv,
            c->drain_pending,
            c->drain_not_before_ns,
            c->drain_epoch_seen,
            zend_hrtime());
        c->drain_pending       = r.drain_pending;
        c->drain_not_before_ns = r.drain_not_before_ns;
        c->drain_epoch_seen    = r.drain_epoch_seen;

        if (r.should_drain) {
            (void)nghttp3_conn_shutdown((nghttp3_conn *)c->nghttp3_conn);
            c->drain_submitted = true;
            http_server_on_h3_goaway_sent(c->counters);
        }
    }

    /* Push the queued response out on this reactor tick instead of waiting
     * for the next inbound datagram. */
    if (c != NULL && !c->closed) {
        http3_connection_drain_out(c);
        http3_connection_arm_timer(c);
    }

    H3T(s->stream_id, "6.drain_done");

    /* Coroutine's reference. After this, only nghttp3's stream_user_data
     * may be holding the stream alive. */
    http3_stream_release(s);
}

/* sendFile hand-off. The static pump (http3_static_response.c) runs as a
 * separate coroutine and submits the response asynchronously, reading
 * s->response_zv live — so the dispose must NOT drop the response zval or
 * release the stream when it hands off; the deferred tail does that from
 * on_done once the pump finishes. Mirrors H2 h2_sendfile_arm/on_done. */
typedef struct {
    http3_connection_t *conn;
    http3_stream_t     *stream;
} h3_sendfile_user_t;

static void h3_sendfile_on_done(void *user, int status)
{
    (void)status;
    h3_sendfile_user_t *u = (h3_sendfile_user_t *)user;
    http3_connection_t *c = u->conn;
    http3_stream_t     *s = u->stream;
    efree(u);

    h3_dispose_tail(c, s);
}

/* Returns true iff the pump took ownership (tail deferred to on_done).
 * On false the response carries a synthesized 500 (or an accounting race);
 * the caller falls through to the regular buffered submit. */
static bool h3_arm_sendfile(http3_connection_t *c, http3_stream_t *s)
{
    http_send_file_request_t *sf_req =
        http_response_take_send_file(Z_OBJ(s->response_zv));

    if (sf_req == NULL) {
        return false;
    }

    h3_sendfile_user_t *u = ecalloc(1, sizeof(*u));
    u->conn   = c;
    u->stream = s;

    if (!http_send_file_dispatch(s->request, Z_OBJ(s->response_zv),
                                 sf_req, h3_sendfile_on_done, u)) {
        efree(u);
        return false;
    }

    return true;
}

static void h3_handler_coroutine_dispose(zend_coroutine_t *coroutine)
{
    http3_stream_t *s = (http3_stream_t *)coroutine->extended_data;

    if (s == NULL) return;

    H3T(s->stream_id, "4.dispose_enter");

    /* Break back-pointers BEFORE doing anything else — a peer
     * RST_STREAM arriving while we're tearing down would otherwise
     * try to ZEND_ASYNC_CANCEL a coroutine that's already
     * disposing. Same order as the H1/H2 dispose paths. */
    coroutine->extended_data = NULL;
    s->coroutine = NULL;

    if (s->request != NULL) s->request->coroutine = NULL;

    http3_connection_t *c = s->conn;

    /* In-flight bracket (paired with on_request_dispatch). */
    if (c != NULL) http_server_on_request_dispose(c->counters);

    /* If the handler threw and never committed a response, derive a
     * 500 from the exception so the peer gets *something*. Mirrors
     * the H2 dispose path's exception → status policy in spirit, but
     * trimmed: we don't rewrite arbitrary 4xx/5xx codes from the
     * exception code field — keep the policy minimal. */
    if (coroutine->exception != NULL && !Z_ISUNDEF(s->response_zv)
        && !http_response_is_committed(Z_OBJ(s->response_zv))) {
        http_response_reset_to_error(Z_OBJ(s->response_zv), 500,
                                     "Internal Server Error");
    }

    if (!Z_ISUNDEF(s->response_zv)
        && !http_response_is_committed(Z_OBJ(s->response_zv))) {
        http_response_set_committed(Z_OBJ(s->response_zv));
    }

    /* Streaming-vs-buffered decision (mirror of H2 dispose).
     *
     * Streaming path: HEADERS were submitted on the first send() via
     * h3_stream_ops.append_chunk; data_reader is already pulling from
     * chunk_queue. All we have to do here is make sure mark_ended fired
     * — if the handler forgot to call $res->end(), do it now so the
     * data_reader emits EOF instead of parking on WOULDBLOCK forever.
     *
     * Buffered path: nothing has been submitted yet; do the headers +
     * single-slice body submit_response now. */
    const bool is_streaming = !Z_ISUNDEF(s->response_zv)
                              && http_response_is_streaming(Z_OBJ(s->response_zv));

    if (c != NULL && !c->closed && c->nghttp3_conn != NULL
        && !Z_ISUNDEF(s->response_zv)) {
        if (is_streaming) {
            H3T(s->stream_id, s->streaming_ended ? "5.streaming_already_ended"
                                                 : "5.streaming_resume");
            if (!s->streaming_ended) {
                s->streaming_ended = true;
                (void)nghttp3_conn_resume_stream(
                    (nghttp3_conn *)c->nghttp3_conn, s->stream_id);
            }
        } else if (http_response_has_send_file(Z_OBJ(s->response_zv))) {
            /* sendFile: hand off to the static pump. On success it owns the
             * stream + response until on_done runs the tail — the pump
             * submits asynchronously and reads response_zv live, so we must
             * NOT fall through to the tail (which would drop it). */
            if (h3_arm_sendfile(c, s)) {
                H3T(s->stream_id, "5.sendfile_armed");
                return;
            }

            /* arm failed → response now carries a synthesized 500; submit it. */
            H3T(s->stream_id, "5.sendfile_failed");
            (void)http3_stream_submit_response(c, s, false);
        } else {
            H3T(s->stream_id, "5.buffered_submit");
            (void)http3_stream_submit_response(c, s, false);
        }
    } else {
        H3T(s->stream_id, "5.SKIP_no_submit");
    }

    h3_dispose_tail(c, s);
}

