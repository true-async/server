/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+

  Worker-side request dispatch for the reactor/worker split (#80, B1b).
  See include/core/worker_dispatch.h.
*/

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include "core/worker_dispatch.h"
#include "php_http_server.h"                 /* http_server_object accessors, response API */
#include "core/http_connection.h"            /* http_request_handler_coroutine_new */
#include "core/http_protocol_handlers.h"     /* http_protocol_get_handler */
#include "http1/http_parser.h"               /* http_request_t, http_request_destroy */
#include "zend_exceptions.h"                 /* zend_clear_exception */
#include "http_response_internal.h"          /* http_response_replace_stream_ops, take_send_file */
#include "http_send_file.h"                  /* http_send_file_request_t (sendFile marshalling) */
#include "core/stream_credit.h"              /* per-stream flow-control credit */
#include "grpc/grpc.h"                       /* grpc_classify */
#include "grpc/grpc_call.h"                  /* call lifecycle policy (init/status/finish) */
#include "Zend/zend_hrtime.h"                /* zend_hrtime — request-service sampling */
#include "log/http_log.h"                    /* access-log emit */

#define WORKER_STREAM_INFLIGHT_CAP (1024 * 1024)

#include <string.h>

/* Defined in src/http_request.c (no public header). Wraps an http_request_t in
 * an HttpRequest zval, taking ownership of the request's single reference. */
extern zval *http_request_create_from_parsed(http_request_t *req);

/* Per-request worker-side dispatch state. Lives from worker_dispatch_request
 * until the handler coroutine's dispose; ecalloc/efree on the worker thread. */
typedef struct {
    http_server_object     *server;
    http_server_counters_t *counters;       /* worker's real counters */
    zval                    request_zv;
    zval                    response_zv;

    /* Request-service sampling: enqueue_ns stamped at dispatch, start_ns at
     * handler entry; on_request_sample feeds sojourn/service to CoDel +
     * telemetry. Gated on sample_stamps_enabled (skips hrtime when no consumer). */
    uint64_t                enqueue_ns;
    uint64_t                start_ns;
    bool                    stamps;

    /* Routing echoed from the request onto the response_wire so the reactor
     * can resolve which QUIC stream to emit on. */
    uint32_t                reactor_id;
    int64_t                 stream_id;
    void                   *conn;

    worker_response_sink_fn sink;
    void                   *sink_arg;

    bool                    skip_handler;  /* synthetic 404 already populated */
    bool                    is_head;       /* suppress the body on render */

    bool                    is_grpc;

    bool                    stream_started;
    bool                    stream_ended;
    bool                    stream_failed;  /* terminal wire becomes STREAM_ABORT */

    stream_credit_t        *credit;         /* shared with the reactor */
    zend_async_trigger_event_t *credit_wake; /* worker-owned; reactor signals it */
    uint64_t                posted_bytes;
} worker_dispatch_ctx_t;

/* Per-request access record (issue #5, B6). The peer rides on the request
 * itself, so the worker logs the client IP even though the connection it was
 * accepted on belongs to the reactor's thread. */
static void worker_log_access(worker_dispatch_ctx_t *ctx)
{
    http_log_state_t *st = http_server_get_log_state(ctx->server);

    if (EXPECTED(!st->has_access) || Z_ISUNDEF(ctx->request_zv)) {
        return;
    }

    http_access_rec_t rec;
    char              ip[INET6_ADDRSTRLEN];

    http_request_fill_access_rec(http_request_from_zobj(Z_OBJ(ctx->request_zv)),
                                 Z_OBJ(ctx->response_zv), &rec, ip, sizeof ip);
    http_log_emit_access(st, &rec);
}

/* Handler coroutine body: run the registered user handler with (request, response). */
static void worker_dispatch_entry(void)
{
    const zend_coroutine_t *const co = ZEND_ASYNC_CURRENT_COROUTINE;
    worker_dispatch_ctx_t *const ctx = (worker_dispatch_ctx_t *)co->extended_data;
    ZEND_ASSERT(ctx != NULL);

    /* Synthetic 404 (no handler) still counts as a served request. */
    if (ctx->skip_handler) {
        http_server_count_request(ctx->counters,
                                  http_response_get_status(Z_OBJ(ctx->response_zv)));
        worker_log_access(ctx);
        return;
    }

    HashTable *const handlers = http_server_get_protocol_handlers(ctx->server);
    zend_fcall_t *const fcall =
        http_protocol_pick_handler(handlers, ctx->is_grpc);

    if (fcall == NULL) {
        return;
    }

    if (ctx->stamps) {
        ctx->start_ns = zend_hrtime();
    }

    zval params[2], retval;
    ZVAL_COPY_VALUE(&params[0], &ctx->request_zv);
    ZVAL_COPY_VALUE(&params[1], &ctx->response_zv);
    ZVAL_UNDEF(&retval);

    zend_fcall_info fci = {
        .size          = sizeof(zend_fcall_info),
        .function_name = fcall->fci.function_name,
        .retval        = &retval,
        .params        = params,
        .object        = NULL,
        .param_count   = 2,
        .named_params  = NULL,
    };

    volatile bool bailout = false;
    zend_try {
        zend_call_function(&fci, &fcall->fci_cache);
    } zend_catch {
        bailout = true;
    } zend_end_try();

    if (UNEXPECTED(bailout)) {
        return;
    }

    /* Stamp end before the retval dtor so destructor time is not charged as
     * service time. */
    http_server_count_request(ctx->counters,
                              http_response_get_status(Z_OBJ(ctx->response_zv)));

    if (ctx->stamps) {
        const uint64_t end_ns = zend_hrtime();
        http_server_on_request_sample(ctx->server,
                                      ctx->start_ns - ctx->enqueue_ns,
                                      end_ns - ctx->start_ns,
                                      end_ns);

        /* The pool stamps the ctx, but the access record reads the request; copy
         * the service window across so http.server.request.duration is present. */
        if (!Z_ISUNDEF(ctx->request_zv)) {
            http_request_t *const req =
                http_request_from_zobj(Z_OBJ(ctx->request_zv));
            req->start_ns = ctx->start_ns;
            req->end_ns   = end_ns;
        }
    }

    worker_log_access(ctx);

    zval_ptr_dtor(&retval);
}

/* Flatten status + H2/H3-allowed headers of the response onto a wire. */
static void worker_wire_copy_head(response_wire_t *rw, zend_object *resp)
{
    int status = http_response_get_status(resp);

    if (UNEXPECTED(status <= 0)) {
        status = 200;
    }

    response_wire_set_status(rw, status);

    HashTable *const headers = http_response_get_headers(resp);

    if (headers == NULL) {
        return;
    }

    zend_string *name;
    zval        *values;
    ZEND_HASH_FOREACH_STR_KEY_VAL(headers, name, values) {
        if (UNEXPECTED(name == NULL)) {
            continue;
        }

        if (!http_response_header_allowed_h2h3(ZSTR_VAL(name), ZSTR_LEN(name))) {
            continue;
        }

        if (EXPECTED(Z_TYPE_P(values) == IS_STRING)) {
            response_wire_add_header(rw, ZSTR_VAL(name), ZSTR_LEN(name),
                                     Z_STRVAL_P(values), Z_STRLEN_P(values));
        } else if (Z_TYPE_P(values) == IS_ARRAY) {
            zval *v;
            ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(values), v) {
                if (Z_TYPE_P(v) != IS_STRING) {
                    continue;
                }

                response_wire_add_header(rw, ZSTR_VAL(name), ZSTR_LEN(name),
                                         Z_STRVAL_P(v), Z_STRLEN_P(v));
            } ZEND_HASH_FOREACH_END();
        }
    } ZEND_HASH_FOREACH_END();
}

/* Flatten the response trailer map onto the wire. */
static void worker_wire_copy_trailers(response_wire_t *rw, zend_object *resp)
{
    HashTable *const trailers = http_response_get_trailers(resp);

    if (trailers == NULL) {
        return;
    }

    zend_string *name;
    zval        *val;
    ZEND_HASH_FOREACH_STR_KEY_VAL(trailers, name, val) {
        if (UNEXPECTED(name == NULL) || Z_TYPE_P(val) != IS_STRING) {
            continue;
        }

        response_wire_add_trailer(rw, ZSTR_VAL(name), ZSTR_LEN(name),
                                  Z_STRVAL_P(val), Z_STRLEN_P(val));
    } ZEND_HASH_FOREACH_END();
}

void response_wire_discard(response_wire_t *rw)
{
    /* nobody adopts the credit ref — release it or the producer hangs */
    stream_credit_abandon((stream_credit_t *)response_wire_credit(rw));

    zend_string *const orphan_chunk =
        (zend_string *)response_wire_take_chunk(rw);

    if (orphan_chunk != NULL) {
        zend_string_release(orphan_chunk);
    }

    response_wire_free(rw);
}

/* The sink owns the wire in every outcome. */
static bool worker_wire_post(worker_dispatch_ctx_t *ctx, response_wire_t *rw)
{
    const response_wire_kind_t kind = response_wire_kind(rw);
    bool delivered = false;

    if (ctx->sink != NULL) {
        delivered = ctx->sink(rw, ctx->sink_arg);
    } else {
        response_wire_discard(rw);
    }

    if (!delivered && kind != RESPONSE_WIRE_FULL) {
        ctx->stream_failed = true;
        http_server_on_worker_wire_dropped(ctx->counters);
    }

    return delivered;
}

/* Park until in-flight < cap; false = stream dead. Suspends on a trigger the
 * reactor signals per ack; a write-timeout timer bounds a peer that stops
 * ACKing. */
static bool worker_stream_wait_credit(worker_dispatch_ctx_t *ctx)
{
    if (ctx->credit == NULL) {
        return true;
    }

    const uint32_t timeout_ms =
        http_server_get_write_timeout_s(ctx->server) * 1000u;

    while (ctx->posted_bytes - stream_credit_acked(ctx->credit)
               >= WORKER_STREAM_INFLIGHT_CAP) {
        if (stream_credit_is_dead(ctx->credit)) {
            return false;
        }

        zend_coroutine_t *const co = ZEND_ASYNC_CURRENT_COROUTINE;

        if (co == NULL || ZEND_ASYNC_IS_SCHEDULER_CONTEXT) {
            return true;   /* can't suspend — degrade to unbounded */
        }

        if (ctx->credit_wake == NULL) {
            ctx->credit_wake = ZEND_ASYNC_NEW_TRIGGER_EVENT();

            if (UNEXPECTED(ctx->credit_wake == NULL)) {
                zend_clear_exception();
                return true;   /* no waker — degrade to unbounded */
            }

            stream_credit_set_waker(ctx->credit, ctx->credit_wake);
            continue;   /* re-check: an ack may have landed pre-publish */
        }

        if (UNEXPECTED(ZEND_ASYNC_WAKER_NEW(co) == NULL)) {
            return false;
        }

        zend_async_resume_when(co, &ctx->credit_wake->base, false,
                               zend_async_waker_callback_resolve, NULL);

        if (timeout_ms > 0) {
            zend_async_event_t *const timer =
                &ZEND_ASYNC_NEW_TIMER_EVENT((zend_ulong)timeout_ms, false)->base;
            zend_async_resume_when(co, timer, true,
                                   zend_async_waker_callback_timeout, NULL);
        }

        ZEND_ASYNC_SUSPEND();
        zend_async_waker_clean(co);

        if (EG(exception) != NULL) {
            return false;   /* write timeout or cancelled while parked */
        }
    }

    return true;
}

static int worker_stream_append_chunk(void *vctx, zend_string *chunk)
{
    worker_dispatch_ctx_t *const ctx = (worker_dispatch_ctx_t *)vctx;

    if (UNEXPECTED(ctx->stream_ended || ctx->stream_failed)
        || (ctx->credit != NULL && stream_credit_is_dead(ctx->credit))) {
        zend_string_release(chunk);
        return HTTP_STREAM_APPEND_STREAM_DEAD;
    }

    /* first send(): open the stream; the reactor adopts one credit ref */
    if (!ctx->stream_started) {
        response_wire_t *const hw =
            response_wire_create(ctx->reactor_id, ctx->stream_id, ctx->conn);

        if (UNEXPECTED(hw == NULL)) {
            zend_string_release(chunk);
            return HTTP_STREAM_APPEND_STREAM_DEAD;
        }

        ctx->credit = stream_credit_create();   /* NULL degrades to unbounded */

        response_wire_set_kind(hw, RESPONSE_WIRE_STREAM_HEADERS);
        response_wire_set_credit(hw, ctx->credit);
        worker_wire_copy_head(hw, Z_OBJ(ctx->response_zv));
        ctx->stream_started = true;

        /* headers undeliverable → the stream never opened; don't copy and
         * post a chunk wire the reactor would only throw away */
        if (UNEXPECTED(!worker_wire_post(ctx, hw))) {
            zend_string_release(chunk);
            return HTTP_STREAM_APPEND_STREAM_DEAD;
        }
    }

    response_wire_t *const cw =
        response_wire_create(ctx->reactor_id, ctx->stream_id, ctx->conn);

    if (UNEXPECTED(cw == NULL)) {
        zend_string_release(chunk);
        return HTTP_STREAM_APPEND_STREAM_DEAD;
    }

    response_wire_set_kind(cw, RESPONSE_WIRE_STREAM_CHUNK);

    /* one copy: ZMM -> persistent; the reactor adopts the ref into its ring */
    zend_string *const pchunk =
        zend_string_init(ZSTR_VAL(chunk), ZSTR_LEN(chunk), 1);

    response_wire_set_chunk(cw, pchunk);

    const size_t chunk_len = ZSTR_LEN(chunk);

    worker_wire_post(ctx, cw);
    zend_string_release(chunk);   /* bytes copied into the wire arena */

    ctx->posted_bytes += chunk_len;

    if (!worker_stream_wait_credit(ctx)) {
        ctx->stream_failed = true;   /* credit timeout / cancelled while parked */
        return HTTP_STREAM_APPEND_STREAM_DEAD;
    }

    return HTTP_STREAM_APPEND_OK;
}

/* sendable() advisory: true while append_chunk would not park. */
static bool worker_stream_sendable(void *vctx)
{
    worker_dispatch_ctx_t *const ctx = (worker_dispatch_ctx_t *)vctx;

    if (ctx->stream_ended) {
        return false;
    }

    if (ctx->credit == NULL) {
        return true;
    }

    return !stream_credit_is_dead(ctx->credit)
           && ctx->posted_bytes - stream_credit_acked(ctx->credit)
                  < WORKER_STREAM_INFLIGHT_CAP;
}

static void worker_stream_mark_ended(void *vctx)
{
    worker_dispatch_ctx_t *const ctx = (worker_dispatch_ctx_t *)vctx;

    if (!ctx->stream_started || ctx->stream_ended) {
        return;
    }

    ctx->stream_ended = true;

    response_wire_t *const ew =
        response_wire_create(ctx->reactor_id, ctx->stream_id, ctx->conn);

    if (UNEXPECTED(ew == NULL)) {
        return;
    }

    if (ctx->stream_failed) {
        response_wire_set_kind(ew, RESPONSE_WIRE_STREAM_ABORT);
    } else {
        response_wire_set_kind(ew, RESPONSE_WIRE_STREAM_END);
        worker_wire_copy_trailers(ew, Z_OBJ(ctx->response_zv));
    }

    worker_wire_post(ctx, ew);
}

static const http_response_stream_ops_t worker_stream_ops = {
    .append_chunk   = worker_stream_append_chunk,
    .sendable       = worker_stream_sendable,
    .mark_ended     = worker_stream_mark_ended,
    .get_wait_event = NULL,   /* backpressure parks inside append_chunk */
};

/* grpc-web in-body trailer frame; consumes the ref. */
static void worker_grpc_append_frame_and_end(void *vctx, zend_string *frame)
{
    worker_dispatch_ctx_t *const ctx = (worker_dispatch_ctx_t *)vctx;

    if (http_response_is_streaming(Z_OBJ(ctx->response_zv))) {
        /* append_chunk consumes the ref (success or failure). */
        if (worker_stream_append_chunk(ctx, frame) == HTTP_STREAM_APPEND_OK) {
            worker_stream_mark_ended(ctx);
        }

        return;
    }

    http_response_static_set_body_str(Z_OBJ(ctx->response_zv), frame);
    zend_string_release(frame);
}

static void worker_grpc_end_stream(void *vctx)
{
    worker_stream_mark_ended(vctx);
}

/* Trailers-Only: dispose posts the FULL wire right after this returns. */
static void worker_grpc_commit(void *vctx)
{
    (void)vctx;
}

static const grpc_finish_ops_t worker_grpc_finish_ops = {
    .append_frame_and_end = worker_grpc_append_frame_and_end,
    .end_stream           = worker_grpc_end_stream,
    .commit               = worker_grpc_commit,
};

/* Flatten the committed HttpResponse into a response_wire. Buffered only.
 * Returns NULL on allocation failure. */
static response_wire_t *worker_render_response(const worker_dispatch_ctx_t *ctx)
{
    zend_object *const resp = Z_OBJ(ctx->response_zv);

    response_wire_t *const rw =
        response_wire_create(ctx->reactor_id, ctx->stream_id, ctx->conn);

    if (rw == NULL) {
        return NULL;
    }

    worker_wire_copy_head(rw, resp);
    worker_wire_copy_trailers(rw, resp);

    /* http_response_get_body_str returns a borrowed reference; the bytes are
     * copied into the arena, so nothing to release. HEAD carries the headers
     * but no body (RFC 9110 §9.3.2). */
    if (!ctx->is_head) {
        zend_string *const body = http_response_get_body_str(resp);

        if (body != NULL && ZSTR_LEN(body) > 0) {
            response_wire_set_body(rw, ZSTR_VAL(body), ZSTR_LEN(body));
        } else {
            response_wire_set_body(rw, NULL, 0);
        }
    } else {
        response_wire_set_body(rw, NULL, 0);
    }

    return rw;
}

/* Marshal $response->sendFile() into a SEND_FILE wire: path + option snapshot
 * as raw bytes; the reactor re-opens the path and runs the sendfile engine
 * (#105). Returns NULL on allocation failure; borrows sf. */
static response_wire_t *worker_render_send_file(const worker_dispatch_ctx_t *ctx,
                                                const http_send_file_request_t *sf)
{
    response_wire_t *const rw =
        response_wire_create(ctx->reactor_id, ctx->stream_id, ctx->conn);

    if (rw == NULL) {
        return NULL;
    }

    response_wire_set_kind(rw, RESPONSE_WIRE_SEND_FILE);

    const http_send_file_options_t *const o = &sf->opts;
    response_wire_send_file_t wsf = {
        .path              = ZSTR_VAL(sf->path),
        .path_len          = ZSTR_LEN(sf->path),
        .content_type      = o->content_type  ? ZSTR_VAL(o->content_type)  : NULL,
        .content_type_len  = o->content_type  ? ZSTR_LEN(o->content_type)  : 0,
        .download_name     = o->download_name ? ZSTR_VAL(o->download_name) : NULL,
        .download_name_len = o->download_name ? ZSTR_LEN(o->download_name) : 0,
        .cache_control     = o->cache_control ? ZSTR_VAL(o->cache_control) : NULL,
        .cache_control_len = o->cache_control ? ZSTR_LEN(o->cache_control) : 0,
        .status            = o->status,
        .disposition       = o->disposition,
        .disposition_set   = o->disposition_set,
        .etag              = o->etag,
        .last_modified     = o->last_modified,
        .accept_ranges     = o->accept_ranges,
        .precompressed     = o->precompressed,
        .conditional       = o->conditional,
        .delete_after_send = o->delete_after_send,
        .is_head           = ctx->is_head,
    };

    if (!response_wire_set_send_file(rw, &wsf)) {
        response_wire_free(rw);
        return NULL;
    }

    return rw;
}

/* Coroutine dispose: commit the response (or derive a 500 from an unhandled
 * exception), render it into a response_wire, hand it to the sink, and drop the
 * per-request state. */
static void worker_dispatch_dispose(zend_coroutine_t *coroutine)
{
    worker_dispatch_ctx_t *const ctx = (worker_dispatch_ctx_t *)coroutine->extended_data;
    ZEND_ASSERT(ctx != NULL);

    coroutine->extended_data = NULL;

    /* Un-bracket the in-flight request (--active), paired with the
     * on_request_dispatch in worker_dispatch_request. */
    http_server_on_request_dispose(ctx->counters);

    /* A thrown handler exception becomes a response (derived 500 below, or
     * a grpc-status / aborted stream) — mark it consumed on both escalation
     * paths so it isn't rethrown into EG and trip a premature graceful
     * shutdown of the worker (#101; see http_handler_coroutine_dispose). */
    if (coroutine->exception != NULL) {
        ZEND_COROUTINE_SET_EXCEPTION_HANDLED(coroutine);
        ZEND_ASYNC_EVENT_SET_EXC_CAUGHT(&coroutine->event);
    }

    if (!Z_ISUNDEF(ctx->response_zv)) {
        zend_object *const resp = Z_OBJ(ctx->response_zv);

        /* gRPC maps exceptions to grpc-status, not HTTP 500 */
        if (coroutine->exception != NULL && !ctx->is_grpc
            && !http_response_is_committed(resp)) {
            http_response_reset_to_error(resp, 500, "Internal Server Error");
        }

        if (ctx->is_grpc) {
            grpc_call_ensure_status(resp, coroutine->exception != NULL);
        }

        if (!http_response_is_committed(resp)) {
            http_response_set_committed(resp);
        }

        if (ctx->is_grpc) {
            grpc_call_finish(resp, &worker_grpc_finish_ops, ctx);
        } else if (http_response_is_streaming(resp)) {
            worker_stream_mark_ended(ctx);
        }

        /* a started stream must always get a terminal wire */
        if (ctx->stream_started && !ctx->stream_ended) {
            worker_stream_mark_ended(ctx);
        }

        if (!ctx->stream_started) {
            /* sendFile() seals the response: marshal path + opts to the
             * reactor, which opens the file and runs the sendfile engine.
             * Falls through to the buffered render when absent (#105). */
            http_send_file_request_t *const sf =
                ctx->is_grpc ? NULL : http_response_take_send_file(resp);

            response_wire_t *rw;

            if (sf != NULL) {
                rw = worker_render_send_file(ctx, sf);
                http_send_file_request_free(sf);
            } else {
                rw = worker_render_response(ctx);
            }

            if (rw != NULL) {
                worker_wire_post(ctx, rw);   /* sink owns rw now */
            }
        }

        /* ctx dies below; a late send() on a kept $response must throw, not UAF */
        http_response_replace_stream_ops(resp, NULL, NULL);
    }

    if (ctx->credit != NULL) {
        if (ctx->credit_wake != NULL) {
            /* retract the waker (fences out an in-flight reactor signal)
             * before disposing its uv_async on this thread */
            stream_credit_clear_waker(ctx->credit);
        }

        stream_credit_release(ctx->credit);   /* worker-side ref */
        ctx->credit = NULL;
    }

    if (ctx->credit_wake != NULL) {
        ZEND_ASYNC_EVENT_SET_CLOSED(&ctx->credit_wake->base);
        ctx->credit_wake->base.dispose(&ctx->credit_wake->base);
        ctx->credit_wake = NULL;
    }

    if (!Z_ISUNDEF(ctx->request_zv)) {
        zval_ptr_dtor(&ctx->request_zv);
        ZVAL_UNDEF(&ctx->request_zv);
    }

    if (!Z_ISUNDEF(ctx->response_zv)) {
        zval_ptr_dtor(&ctx->response_zv);
        ZVAL_UNDEF(&ctx->response_zv);
    }

    efree(ctx);
}

bool worker_dispatch_request(http_server_object *server,
                             zend_async_scope_t *scope,
                             http_request_t *req,
                             const bool own_scope,
                             worker_response_sink_fn sink, void *sink_arg)
{
    if (UNEXPECTED(server == NULL || scope == NULL || req == NULL)) {
        if (req != NULL) {
            http_request_destroy(req);  /* we own it; nothing else can free it */
        }

        return false;
    }

    /* Routing must be read before create_from_parsed: on the coroutine-spawn
     * failure path below the object owns (and may free) req. */
    const uint32_t reactor_id = req->reactor_id;
    const int64_t  stream_id  = req->reactor_stream_id;
    void *const    conn       = req->reactor_conn;
    const bool     is_head    = http_request_method_is_head(req);

    HashTable *const handlers = http_server_get_protocol_handlers(server);
    const grpc_mode_t grpc_mode = grpc_classify(req, handlers);
    const bool is_grpc = grpc_mode != GRPC_MODE_NONE;

    zval *const req_obj = http_request_create_from_parsed(req);

    if (UNEXPECTED(req_obj == NULL)) {
        http_request_destroy(req);  /* nothing took the ref yet */
        return false;
    }

    worker_dispatch_ctx_t *const ctx = ecalloc(1, sizeof(*ctx));
    ctx->server     = server;
    ctx->counters   = http_server_counters(server);
    ctx->stamps     = http_server_sample_stamps_enabled(http_server_view(server));
    ctx->reactor_id = reactor_id;
    ctx->stream_id  = stream_id;
    ctx->conn       = conn;
    ctx->sink       = sink;
    ctx->sink_arg   = sink_arg;
    ctx->is_head    = is_head;
    ctx->is_grpc    = is_grpc;

    ZVAL_COPY_VALUE(&ctx->request_zv, req_obj);
    efree(req_obj);                 /* the heap zval wrapper, not the object */

    object_init_ex(&ctx->response_zv, http_response_ce);
    http_response_set_protocol_version(Z_OBJ(ctx->response_zv), "3.0");
    http_response_set_head(Z_OBJ(ctx->response_zv), is_head);

    http_response_install_stream_ops(Z_OBJ(ctx->response_zv),
                                     &worker_stream_ops, ctx);

    if (is_grpc) {
        grpc_call_init_response(Z_OBJ(ctx->response_zv), grpc_mode);
    }

    /* No handler registered: synthesise a 404 so the sink still fires with a
     * response instead of leaving the stream hanging. */
    zend_fcall_t *const fcall = http_protocol_pick_handler(handlers, is_grpc);

    if (fcall == NULL) {
        http_response_static_set_status(Z_OBJ(ctx->response_zv), 404);
        http_response_static_set_header(Z_OBJ(ctx->response_zv),
            "content-type", 12, "text/plain; charset=utf-8", 25);
        zend_string *const msg = zend_string_init("Not Found", 9, 0);
        http_response_static_set_body_str(Z_OBJ(ctx->response_zv), msg);
        zend_string_release(msg);
        ctx->skip_handler = true;
    }

    zend_coroutine_t *const co = http_request_handler_coroutine_new(
        scope, worker_dispatch_entry, ctx, worker_dispatch_dispose, own_scope);

    if (UNEXPECTED(co == NULL)) {
        zval_ptr_dtor(&ctx->request_zv);
        zval_ptr_dtor(&ctx->response_zv);
        efree(ctx);
        return false;
    }

    /* Bracket the in-flight request on the worker's counters (++active),
     * paired with on_request_dispose at coroutine dispose. */
    http_server_on_request_dispatch(ctx->counters);

    if (ctx->stamps) {
        ctx->enqueue_ns = zend_hrtime();
    }

    ZEND_ASYNC_ENQUEUE_COROUTINE(co);

    return true;
}
