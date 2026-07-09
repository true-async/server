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
#include "core/stream_credit.h"             /* reverse-path flow control */
#include "grpc/grpc.h"                      /* gRPC request classification */
#include "grpc/grpc_call.h"                 /* gRPC call lifecycle policy */
#include "static/static_handler.h"         /* http_static_try_serve / count */
#include "core/response_wire.h"             /* response_wire_* (reverse path) */
#include "core/worker_dispatch.h"           /* response_wire_discard */

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
/* Inbox backlog (undrained requests) at which a connection's home worker is
 * considered busy and a request spills to a less-loaded worker. Well below
 * WORKER_INBOX_CAPACITY (1024) so spill kicks in before hard backpressure; -D-overridable. */
#ifndef H3_WORKER_SPILL_DEPTH
#define H3_WORKER_SPILL_DEPTH 64
#endif

/* Reactor mode: hand the parsed request to a PHP worker by pointer
 * instead of spawning a handler coroutine here on the transport thread. The
 * embedded persistent http_request_t crosses to the worker; the worker reads
 * it, runs the handler, and posts the response + consumed back over the reverse
 * channel. The reactor keeps the stream alive via a worker-borrow ref until the
 * consumed arrives. No request-service stats here — those are the
 * worker's job (handler runs there). */
static void http3_stream_dispatch_to_worker(http3_connection_t *c, http3_stream_t *s,
                                             const http3_reactor_ctx_t *rctx)
{
    /* Reactor-paired sticky dispatch. A connection homes to one of this
     * reactor's owned workers and reuses it for all its streams (locality); a home
     * that backs up past H3_WORKER_SPILL_DEPTH spills this request to a less-loaded
     * worker (owned first, else any), and a home whose worker died is re-homed. */
    worker_registry_t *const reg = rctx->registry;
    int slot = c->worker_slot;
    worker_inbox_t *inbox = slot >= 0 ? worker_registry_at(reg, slot) : NULL;

    if (inbox == NULL) {
        inbox = worker_registry_least_busy(reg, rctx->reactor_id, rctx->n_reactors, &slot);

        if (inbox == NULL) {
            inbox = worker_registry_least_busy(reg, -1, rctx->n_reactors, &slot);
        }

        if (inbox != NULL) {
            c->worker_slot = slot;
        }
    }

    if (inbox != NULL && worker_inbox_depth(inbox) >= H3_WORKER_SPILL_DEPTH) {
        int spill_slot;
        worker_inbox_t *alt =
            worker_registry_least_busy(reg, rctx->reactor_id, rctx->n_reactors, &spill_slot);

        if (alt == NULL || worker_inbox_depth(alt) >= H3_WORKER_SPILL_DEPTH) {
            worker_inbox_t *const any =
                worker_registry_least_busy(reg, -1, rctx->n_reactors, &spill_slot);

            if (any != NULL) {
                alt = any;
            }
        }

        if (alt != NULL) {
            inbox = alt;
        }
    }

    if (inbox == NULL) {
        /* No worker published yet — leave the stream undispatched. It is torn
         * down on the normal QUIC lifecycle (client PTO/RST); reactor teardown
         * frees the request fields (s->dispatched stays false). */
        return;
    }

    s->conn = c;

    /* Routing for the reverse path. reactor_id selects the reverse channel;
     * reactor_conn carries the raw stream pointer (kept alive by the
     * worker-borrow ref below until consumed, so it is valid when the response
     * comes back); stream_id is for validation/logging. The raw pointer becomes
     * a generationed handle when validate-and-drop lands. */
    s->request->reactor_id        = (uint32_t)rctx->reactor_id;
    s->request->reactor_stream_id = s->stream_id;
    s->request->reactor_conn      = s;

    s->dispatched = true;
    s->refcount++;   /* worker-borrow ref; dropped by the consumed apply */

    if (UNEXPECTED(!worker_inbox_post(inbox, s->request))) {
        /* inbox full: undo the dispatch bookkeeping, RESET with
         * H3_REQUEST_REJECTED so the client can retry instead of hanging */
        s->refcount--;
        s->dispatched = false;

        if (c->ngtcp2_conn != NULL) {
            (void)ngtcp2_conn_shutdown_stream_write(
                (ngtcp2_conn *)c->ngtcp2_conn, 0, s->stream_id,
                NGHTTP3_H3_REQUEST_REJECTED);
            http3_listener_mark_flush(c->listener, c);
            http3_listener_queue_epilogue_flush(c->listener);
        }

        return;
    }

    H3T(s->stream_id, "1.dispatch_to_worker");
}

/* Reverse path: apply a worker-rendered response_wire on the reactor
 * thread (posted via reactor_pool_post_exec). The wire carries the raw stream
 * pointer (response_wire_conn) — valid here because the worker-borrow ref keeps
 * the stream alive until the consumed that follows the response (FIFO on one
 * reactor mailbox). If the stream/connection is already gone (client RST), the
 * lookup is the validate-and-drop point: free the wire and return. On success,
 * QPACK-encode + submit, then drain it out on this tick. Takes ownership of the
 * wire. */
typedef struct { http3_connection_t *conn; http3_stream_t *stream; } h3_sendfile_user_t;

extern void http3_connection_drain_out(http3_connection_t *);
extern void http3_connection_arm_timer(http3_connection_t *);

/* Rebuild a PHP-side sendFile request from the flat wire snapshot. The
 * reactor owns the returned req; http_send_file_dispatch frees it. */
static http_send_file_request_t *h3_send_file_req_from_wire(const response_wire_send_file_t *w)
{
    http_send_file_request_t *const req = ecalloc(1, sizeof(*req));

    req->path = zend_string_init(w->path, w->path_len, 0);

    if (w->content_type != NULL) {
        req->opts.content_type = zend_string_init(w->content_type, w->content_type_len, 0);
    }
    if (w->download_name != NULL) {
        req->opts.download_name = zend_string_init(w->download_name, w->download_name_len, 0);
    }
    if (w->cache_control != NULL) {
        req->opts.cache_control = zend_string_init(w->cache_control, w->cache_control_len, 0);
    }

    req->opts.status            = w->status;
    req->opts.disposition       = w->disposition;
    req->opts.disposition_set   = w->disposition_set;
    req->opts.etag              = w->etag;
    req->opts.last_modified     = w->last_modified;
    req->opts.accept_ranges     = w->accept_ranges;
    req->opts.precompressed     = w->precompressed;
    req->opts.conditional       = w->conditional;
    req->opts.delete_after_send = w->delete_after_send;

    return req;
}

/* Retire the reactor-side response object and flush. The slab slot is
 * reclaimed by the normal pool release (nghttp3 close + worker consumed),
 * which needs response_zv UNDEF — so dtor it here. */
static void h3_reactor_sendfile_cleanup(http3_connection_t *c, http3_stream_t *s)
{
    if (!Z_ISUNDEF(s->response_zv)) {
        zval_ptr_dtor(&s->response_zv);
        ZVAL_UNDEF(&s->response_zv);
    }

    if (c != NULL && !c->closed) {
        http3_connection_drain_out(c);
        http3_connection_arm_timer(c);
    }
}

/* Pump completion for a pooled sendFile. Does NOT release the stream — the
 * base (nghttp3) + worker-borrow refs already cover its lifetime. */
static void h3_reactor_sendfile_on_done(void *user, int status)
{
    (void)status;
    h3_sendfile_user_t *const u = (h3_sendfile_user_t *)user;
    http3_connection_t *const c = u->conn;
    http3_stream_t     *const s = u->stream;
    efree(u);

    h3_reactor_sendfile_cleanup(c, s);
}

/* SEND_FILE apply: rebuild a reactor-side response + request from the wire and
 * run the shared sendfile engine (same call as the non-pool H3 path). The
 * engine opens the file here and owns/closes the fd. On dispatch failure the
 * response carries a 500 — submit it before retiring the object. */
static void h3_reactor_apply_send_file(http3_connection_t *c, http3_stream_t *s,
                                       response_wire_t *rw)
{
    response_wire_send_file_t w;

    if (!response_wire_get_send_file(rw, &w)) {
        return;
    }

    object_init_ex(&s->response_zv, http_response_ce);
    http_response_set_protocol_version(Z_OBJ(s->response_zv), "3.0");
    http_response_set_head(Z_OBJ(s->response_zv), w.is_head);
    /* The engine drives delivery through the protocol op — same ops the
     * non-pool H3 dispatch installs (send_static_response = the pump). */
    http_response_install_stream_ops(Z_OBJ(s->response_zv), &h3_stream_ops, s);

    http_send_file_request_t *const req = h3_send_file_req_from_wire(&w);

    h3_sendfile_user_t *const u = ecalloc(1, sizeof(*u));
    u->conn   = c;
    u->stream = s;

    if (http_send_file_dispatch(s->request, Z_OBJ(s->response_zv), req,
                                h3_reactor_sendfile_on_done, u)) {
        return;   /* pump owns delivery; on_done retires the response object */
    }

    /* dispatch failed: the response carries a synthesized 500. on_done was
     * not fired, so we own u; submit the error, then retire the object. */
    efree(u);
    (void)http3_stream_submit_response(c, s, false);
    h3_reactor_sendfile_cleanup(c, s);
}

void http3_reactor_apply_response(void *arg)
{
    response_wire_t *const rw = (response_wire_t *)arg;

    if (rw == NULL) {
        return;
    }

    http3_stream_t *const s = (http3_stream_t *)response_wire_conn(rw);
    http3_connection_t *const c = (s != NULL) ? s->conn : NULL;

    if (c == NULL || c->closed || c->nghttp3_conn == NULL) {
        /* stream gone: abandon credit / release chunk — unblock the producer */
        response_wire_discard(rw);
        return;
    }

    /* mark dirty + flush once in the drain epilogue, not per wire */
    switch (response_wire_kind(rw)) {
        case RESPONSE_WIRE_FULL:
        case RESPONSE_WIRE_STREAM_HEADERS:
            if (http3_stream_submit_response_wire(c, s, rw)) {
                http3_listener_mark_flush(c->listener, c);
                http3_listener_queue_epilogue_flush(c->listener);
            }
            break;

        case RESPONSE_WIRE_STREAM_CHUNK: {
            zend_string *const chunk =
                (zend_string *)response_wire_take_chunk(rw);

            if (chunk == NULL) {
                break;
            }

            if (s->peer_closed || s->streaming_ended || s->chunk_queue == NULL) {
                zend_string_release(chunk);
                break;
            }

            h3_chunk_queue_push(s, chunk);
            http_server_on_stream_send(c->counters, ZSTR_LEN(chunk));

            (void)nghttp3_conn_resume_stream(
                (nghttp3_conn *)c->nghttp3_conn, s->stream_id);
            http3_listener_mark_flush(c->listener, c);
            http3_listener_queue_epilogue_flush(c->listener);
            break;
        }

        case RESPONSE_WIRE_STREAM_END:
            if (s->peer_closed || s->streaming_ended || s->chunk_queue == NULL) {
                break;
            }

            http3_stream_adopt_wire_trailers(s, rw);
            s->streaming_ended = true;

            (void)nghttp3_conn_resume_stream(
                (nghttp3_conn *)c->nghttp3_conn, s->stream_id);
            http3_listener_mark_flush(c->listener, c);
            http3_listener_queue_epilogue_flush(c->listener);
            break;

        case RESPONSE_WIRE_STREAM_ABORT:
            if (s->peer_closed || s->streaming_ended) {
                break;
            }

            s->streaming_ended = true;

            if (c->ngtcp2_conn != NULL) {
                (void)ngtcp2_conn_shutdown_stream_write(
                    (ngtcp2_conn *)c->ngtcp2_conn, 0, s->stream_id,
                    NGHTTP3_H3_INTERNAL_ERROR);
            }

            http3_listener_mark_flush(c->listener, c);
            http3_listener_queue_epilogue_flush(c->listener);
            break;

        case RESPONSE_WIRE_SEND_FILE:
            h3_reactor_apply_send_file(c, s, rw);
            break;
    }

    response_wire_free(rw);
}

/* Reactor-side static serving: serve files
 * entirely on the transport reactor — no PHP, no worker round-trip. Returns
 * true when the static FSM claimed the request (HANDLED inline / HARD_ZERO
 * sendfile), false on PASSTHROUGH so the caller routes to a worker. The
 * response is built and submitted in the reactor's own ZMM; s->request is
 * read-only here (persistent), freed by the reactor on stream release. */
static bool http3_reactor_try_static(http3_connection_t *c, http3_stream_t *s,
                                     const http3_reactor_ctx_t *rctx)
{
    if (rctx->static_mount_count == 0 || rctx->static_mounts == NULL) {
        return false;
    }

    object_init_ex(&s->response_zv, http_response_ce);
    http_response_set_protocol_version(Z_OBJ(s->response_zv), "3.0");
    http_response_set_head(Z_OBJ(s->response_zv),
                           http_request_method_is_head(s->request));

    const http_static_result_t rc = http_static_try_serve_mounts(
        (const http_static_handler_t *const *)rctx->static_mounts,
        rctx->static_mount_count, rctx->static_cache,
        s->request, Z_OBJ(s->response_zv), c->counters,
        &h3_static_dispatch_cbs, s);

    if (rc == HTTP_STATIC_PASSTHROUGH) {
        zval_ptr_dtor(&s->response_zv);
        ZVAL_UNDEF(&s->response_zv);
        return false;
    }

    if (rc == HTTP_STATIC_HARD_ZERO) {
        /* Sendfile pump owns the stream: on_armed pinned s->refcount + the
         * in-flight counter; on_static_done runs h3_dispose_tail. */
        return true;
    }

    /* HANDLED: inline body / 4xx. Take a serving ref (mirrors the local
     * coroutine ref) so the slab survives the dispose_tail release until
     * nghttp3 closes the stream, then submit + drain. */
    s->refcount++;

    if (!c->closed && c->nghttp3_conn != NULL) {
        (void)http3_stream_submit_response(c, s, false);
    }

    h3_dispose_tail(c, s);
    return true;
}

void http3_stream_dispatch(http3_connection_t *c, http3_stream_t *s)
{
    if (c == NULL || s == NULL || s->dispatched) {
        return;
    }

    ZEND_ASSERT(s->request != NULL);

    /* Reactor mode: serve static here on the transport thread; otherwise route
     * the request to a PHP worker instead of dispatching locally. */
    const http3_reactor_ctx_t *const rctx = http3_listener_reactor_ctx(c->listener);

    if (rctx != NULL) {
        if (http3_reactor_try_static(c, s, rctx)) {
            return;
        }

        http3_stream_dispatch_to_worker(c, s, rctx);
        return;
    }

    http_server_object *server =
        (http_server_object *)http3_listener_server_obj(c->listener);

    if (server == NULL) {
        return;          /* unit-test path — no PHP context to dispatch into */
    }

    HashTable *handlers = http_server_get_protocol_handlers(server);
    zend_async_scope_t *scope = http_server_get_scope(server);

    const grpc_mode_t grpc_mode = grpc_classify(s->request, handlers);

    s->is_grpc = grpc_mode != GRPC_MODE_NONE;

    zend_fcall_t *fcall = http_protocol_pick_handler(handlers, s->is_grpc);

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
    http_response_set_head(Z_OBJ(s->response_zv),
                           http_request_method_is_head(s->request));
    /* Wire the streaming vtable so HttpResponse::send() in the
     * handler enqueues into our chunk_queue. setBody/end (REST) handlers
     * never touch this; they go through the buffered submit_response in
     * dispose. */
    http_response_install_stream_ops(Z_OBJ(s->response_zv),
                                     &h3_stream_ops, s);

    if (s->is_grpc) {
        grpc_call_init_response(Z_OBJ(s->response_zv), grpc_mode);
    }

#ifdef HAVE_HTTP_COMPRESSION
    /* Attach compression state. Server pointer comes from
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

    /* Static-handler dispatch. Same policy as the H1/H2
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
        scope, h3_handler_coroutine_entry, s, h3_handler_coroutine_dispose,
        s->conn->view != NULL ? s->conn->view->request_scope : true);

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
    ZEND_ASSERT(s != NULL);

    if (s->conn == NULL) return;

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
    zend_fcall_t *fcall = http_protocol_pick_handler(handlers, s->is_grpc);

    if (fcall == NULL) return;

#ifdef HAVE_HTTP_COMPRESSION
    /* Inbound Content-Encoding decode. Same shape as the
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
 * on_done once the pump finishes. Mirrors H2 h2_sendfile_arm/on_done.
 * (h3_sendfile_user_t is declared near http3_reactor_apply_response.) */
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

/* End a streamed reply. Trailers must be captured here, while response_zv
 * is still alive — the data reader runs after dispose frees the zvals. */
static void h3_stream_finish_streaming(void *ctx)
{
    http3_stream_t *s = (http3_stream_t *)ctx;

    http3_stream_capture_trailers(s);

    H3T(s->stream_id, s->streaming_ended ? "5.streaming_already_ended"
                                         : "5.streaming_resume");
    if (!s->streaming_ended) {
        s->streaming_ended = true;
        (void)nghttp3_conn_resume_stream((nghttp3_conn *)s->conn->nghttp3_conn,
                                         s->stream_id);
    }
}

/* Append one gRPC frame (consumes the ref) and end the stream. */
static void h3_grpc_append_frame_and_end(void *ctx, zend_string *frame)
{
    http3_stream_t *s = (http3_stream_t *)ctx;

    (void)h3_stream_ops.append_chunk(s, frame);   /* consumes the ref */

    h3_stream_finish_streaming(s);
}

/* Buffered commit — single HEADERS + fin (the Trailers-Only shape). */
static void h3_grpc_commit(void *ctx)
{
    http3_stream_t     *s = (http3_stream_t *)ctx;
    http3_connection_t *c = s->conn;

    if (c != NULL && !c->closed && c->nghttp3_conn != NULL) {
        (void)http3_stream_submit_response(c, s, false);
    }
}

static const grpc_finish_ops_t h3_grpc_finish_ops = {
    .append_frame_and_end = h3_grpc_append_frame_and_end,
    .end_stream           = h3_stream_finish_streaming,
    .commit               = h3_grpc_commit,
};

static void h3_handler_coroutine_dispose(zend_coroutine_t *coroutine)
{
    http3_stream_t *s = (http3_stream_t *)coroutine->extended_data;
    ZEND_ASSERT(s != NULL);

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

    /* A thrown handler exception becomes a response (derived 500 below, or
     * a grpc-status / aborted stream) — mark it consumed on both escalation
     * paths so it isn't rethrown into EG and trip a premature graceful
     * shutdown of the whole worker (#101; see http_handler_coroutine_dispose). */
    if (coroutine->exception != NULL) {
        ZEND_COROUTINE_SET_EXCEPTION_HANDLED(coroutine);
        ZEND_ASYNC_EVENT_SET_EXC_CAUGHT(&coroutine->event);
    }

    /* If the handler threw and never committed a response, derive a
     * 500 from the exception so the peer gets *something*. Mirrors
     * the H2 dispose path's exception → status policy in spirit, but
     * trimmed: we don't rewrite arbitrary 4xx/5xx codes from the
     * exception code field — keep the policy minimal. */
    if (coroutine->exception != NULL && !s->is_grpc && !Z_ISUNDEF(s->response_zv)
        && !http_response_is_committed(Z_OBJ(s->response_zv))) {
        http_response_reset_to_error(Z_OBJ(s->response_zv), 500,
                                     "Internal Server Error");
    }

    if (!Z_ISUNDEF(s->response_zv)
        && !http_response_is_committed(Z_OBJ(s->response_zv))) {
        http_response_set_committed(Z_OBJ(s->response_zv));
    }

    /* gRPC outcome → grpc-status trailer (policy in src/grpc/grpc_call.c). */
    if (s->is_grpc && !Z_ISUNDEF(s->response_zv)) {
        grpc_call_ensure_status(Z_OBJ(s->response_zv),
                                coroutine->exception != NULL);
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
        if (s->is_grpc) {
            /* Delivery shape is gRPC policy — grpc_call_finish decides. */
            grpc_call_finish(Z_OBJ(s->response_zv), &h3_grpc_finish_ops, s);
        } else if (is_streaming) {
            h3_stream_finish_streaming(s);
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
            /* capture trailers while response_zv is alive */
            http3_stream_capture_trailers(s);
            (void)http3_stream_submit_response(c, s, false);
        }
    } else {
        H3T(s->stream_id, "5.SKIP_no_submit");
    }

    h3_dispose_tail(c, s);
}

