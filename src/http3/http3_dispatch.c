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

/* Defined in src/http_request.c. Declared here because the public
 * php_http_server.h header doesn't expose it (it lives in the C boundary
 * between the parser and the PHP HttpRequest object). */
extern zval *http_request_create_from_parsed(http_request_t *req);

/* http3_listener_server_obj already declared in http3_listener.h. */

/* Forward decls for the entry / dispose pair, mutually referenced
 * through the coroutine vtable. */
static void h3_handler_coroutine_entry(void);
static void h3_handler_coroutine_dispose(zend_coroutine_t *coroutine);

/* Full user-handler dispatch.
 *
 * Called from h3_end_stream_cb once the request is fully assembled.
 * Builds the per-stream PHP zvals (HttpRequest, HttpResponse), spawns
 * a TrueAsync coroutine in the server scope so the handler can suspend
 * (await), and hands off. The coroutine's dispose path serialises the
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
    if (fcall == NULL || scope == NULL) {
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

    /* Spawn the per-stream handler coroutine. extended_data is the
     * STREAM (not the connection) — that's how N concurrent streams on
     * the same QUIC connection get N independent (request, response)
     * zval pairs. */
    zend_coroutine_t *co = ZEND_ASYNC_NEW_COROUTINE(scope);
    if (co == NULL) {
        zval_ptr_dtor(&s->request_zv);  ZVAL_UNDEF(&s->request_zv);
        zval_ptr_dtor(&s->response_zv); ZVAL_UNDEF(&s->response_zv);
        s->dispatched = false;
        return;
    }
    co->internal_entry   = h3_handler_coroutine_entry;
    co->extended_data    = s;
    co->extended_dispose = h3_handler_coroutine_dispose;

    s->coroutine = co;
    /* Coroutine holds its own ref. Released in dispose. */
    s->refcount++;

    /* Bracket on the server's in-flight counter — admission / CoDel
     * see H3 load at the right granularity. Paired with on_request_dispose
     * in h3_handler_coroutine_dispose. */
    http_server_on_request_dispatch(s->conn->counters);

    s->request->coroutine   = co;
    s->request->enqueue_ns  = zend_hrtime();

    ZEND_ASYNC_ENQUEUE_COROUTINE(co);
}

static void h3_handler_coroutine_entry(void)
{
    const zend_coroutine_t *co = ZEND_ASYNC_CURRENT_COROUTINE;
    http3_stream_t *s = (http3_stream_t *)co->extended_data;
    if (s == NULL || s->conn == NULL) return;

    if (s->request != NULL) {
        s->request->start_ns = zend_hrtime();
    }

    http_server_object *server =
        (http_server_object *)http3_listener_server_obj(s->conn->listener);
    HashTable *handlers = http_server_get_protocol_handlers(server);
    zend_fcall_t *fcall = http_protocol_get_handler(handlers, HTTP_PROTOCOL_HTTP1);
    if (fcall == NULL) {
        fcall = http_protocol_get_handler(handlers, HTTP_PROTOCOL_HTTP2);
    }
    if (fcall == NULL) return;

    zval params[2], retval;
    ZVAL_COPY_VALUE(&params[0], &s->request_zv);
    ZVAL_COPY_VALUE(&params[1], &s->response_zv);
    ZVAL_UNDEF(&retval);

    call_user_function(NULL, NULL, &fcall->fci.function_name,
                       &retval, 2, params);

    /* Stamp end_ns + feed backpressure sample BEFORE retval dtor so
     * destructor time on a returned object doesn't get counted as
     * service time. Same discipline as H1/H2 handler entries. */
    if (s->request != NULL && server != NULL) {
        s->request->end_ns = zend_hrtime();
        http_server_on_request_sample(
            server,
            s->request->start_ns - s->request->enqueue_ns,
            s->request->end_ns   - s->request->start_ns);
    }
    zval_ptr_dtor(&retval);
}

static void h3_handler_coroutine_dispose(zend_coroutine_t *coroutine)
{
    http3_stream_t *s = (http3_stream_t *)coroutine->extended_data;
    if (s == NULL) return;

    /* Break back-pointers BEFORE doing anything else — a peer
     * RST_STREAM arriving while we're tearing down would otherwise
     * try to ZEND_ASYNC_CANCEL a coroutine that's already
     * disposing. Same order as the H1/H2 dispose paths. */
    coroutine->extended_data = NULL;
    s->coroutine = NULL;
    if (s->request != NULL) s->request->coroutine = NULL;

    http3_connection_t *c = s->conn;
    http_server_object *server = (c != NULL && c->listener != NULL)
        ? (http_server_object *)http3_listener_server_obj(c->listener) : NULL;

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
            if (!s->streaming_ended) {
                s->streaming_ended = true;
                (void)nghttp3_conn_resume_stream(
                    (nghttp3_conn *)c->nghttp3_conn, s->stream_id);
            }
        } else {
            (void)http3_stream_submit_response(c, s, false);
        }
    }

    /* Drop per-stream zvals — we no longer need them. The data_reader
     * pulls from s->response_body (buffered REST) or from chunk_queue
     * (streaming) — both have lifetimes independent of the response
     * zval, so releasing it here is safe. */
    if (!Z_ISUNDEF(s->request_zv)) {
        zval_ptr_dtor(&s->request_zv);
        ZVAL_UNDEF(&s->request_zv);
    }
    if (!Z_ISUNDEF(s->response_zv)) {
        zval_ptr_dtor(&s->response_zv);
        ZVAL_UNDEF(&s->response_zv);
    }

    /* Graceful drain check at the response-commit point — matches H1/H2
     * drain. If proactive age has elapsed or a reactive drain epoch fired,
     * submit an HTTP/3 GOAWAY via nghttp3_conn_shutdown so new streams
     * are refused while the in-flight one finishes. drain_submitted
     * guards against re-emitting on subsequent commits over the same
     * connection. */
    if (c != NULL && !c->closed && !c->drain_submitted
        && c->nghttp3_conn != NULL) {
        http_server_object *srv =
            (http_server_object *)http3_listener_server_obj(c->listener);
        const http_server_drain_eval_t r = http_server_drain_evaluate(srv,
            c->drain_pending,
            c->drain_not_before_ns,
            c->drain_epoch_seen);
        c->drain_pending       = r.drain_pending;
        c->drain_not_before_ns = r.drain_not_before_ns;
        c->drain_epoch_seen    = r.drain_epoch_seen;
        if (r.should_drain) {
            (void)nghttp3_conn_shutdown((nghttp3_conn *)c->nghttp3_conn);
            c->drain_submitted = true;
            http_server_on_h3_goaway_sent(c->counters);
        }
    }

    /* Push the queued response out to the wire. Submit_response only
     * queues into nghttp3; the next read_pkt → drain cycle would
     * otherwise be the soonest opportunity. Triggering drain here
     * means the response leaves on the same reactor tick the handler
     * completed, instead of waiting for the next inbound datagram. */
    if (c != NULL && !c->closed) {
        http3_connection_drain_out(c);
        http3_connection_arm_timer(c);
    }

    /* Coroutine's reference. After this, only nghttp3's
     * stream_user_data may be holding the stream alive. */
    http3_stream_release(s);
}

