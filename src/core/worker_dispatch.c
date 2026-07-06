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
#include "core/stream_credit.h"              /* per-stream flow-control credit */
#include "grpc/grpc.h"                       /* grpc_request_is_grpc / _is_grpc_web */
#include "grpc/grpc_call.h"                  /* call lifecycle policy (init/status/finish) */
#include "Zend/zend_hrtime.h"                /* zend_hrtime — request-service sampling */

/* Streaming reverse-path flow control: how many un-acked bytes a stream may
 * have in flight before append_chunk parks the producer coroutine, and how
 * often a parked producer re-reads the credit counter. */
#define WORKER_STREAM_INFLIGHT_CAP (1024 * 1024)
#define WORKER_CREDIT_POLL_MS      2

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

    /* gRPC classification (once, at dispatch — the transports do the same). */
    bool                    is_grpc;
    bool                    grpc_web;

    /* Streaming reverse path: STREAM_HEADERS posted on the first
     * append_chunk; STREAM_END posted by mark_ended (idempotent). When
     * stream_started, dispose skips the buffered FULL render. */
    bool                    stream_started;
    bool                    stream_ended;

    /* Flow control (step 3): `credit` is shared with the reactor (see
     * stream_credit.h); posted_bytes is worker-local. In-flight =
     * posted_bytes - acked; append parks over WORKER_STREAM_INFLIGHT_CAP. */
    stream_credit_t        *credit;
    uint64_t                posted_bytes;
} worker_dispatch_ctx_t;

/* Handler coroutine body: run the registered user handler with (request, response). */
static void worker_dispatch_entry(void)
{
    const zend_coroutine_t *const co = ZEND_ASYNC_CURRENT_COROUTINE;
    worker_dispatch_ctx_t *const ctx = (worker_dispatch_ctx_t *)co->extended_data;
    ZEND_ASSERT(ctx != NULL);

    /* Synthetic 404 (no handler) still counts as a served request. */
    if (ctx->skip_handler) {
        http_server_count_request(ctx->counters);
        return;
    }

    HashTable *const handlers = http_server_get_protocol_handlers(ctx->server);
    zend_fcall_t *fcall = ctx->is_grpc
        ? http_protocol_get_handler(handlers, HTTP_PROTOCOL_GRPC)
        : NULL;

    if (fcall == NULL) {
        fcall = http_protocol_get_handler(handlers, HTTP_PROTOCOL_HTTP1);
    }

    if (fcall == NULL) {
        fcall = http_protocol_get_handler(handlers, HTTP_PROTOCOL_HTTP2);
    }

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
    http_server_count_request(ctx->counters);

    if (ctx->stamps) {
        const uint64_t end_ns = zend_hrtime();
        http_server_on_request_sample(ctx->server,
                                      ctx->start_ns - ctx->enqueue_ns,
                                      end_ns - ctx->start_ns,
                                      end_ns);
    }

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

/* Flatten the trailer map (setTrailer / gRPC grpc-status) onto a wire — flat
 * string pairs, same discipline as http3_stream_capture_trailers locally. */
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

/* Hand a wire to the reverse channel. The sink owns it on success. Stream
 * kinds must not be silently dropped mid-stream, so the sink (which retries
 * on a transiently full mailbox) is the single delivery point. */
static void worker_wire_post(worker_dispatch_ctx_t *ctx, response_wire_t *rw)
{
    if (ctx->sink != NULL) {
        ctx->sink(rw, ctx->sink_arg);
    } else {
        response_wire_free(rw);
    }
}

/* ---------------------------------------------------------------------
 * Streaming reverse path (worker side of #80 D3, streaming leg).
 *
 * The worker's HttpResponse gets these stream ops so send()/writeMessage()
 * work under the pool: each op flattens its payload into a STREAM_* wire
 * and posts it to the originating reactor. Ordering is the mailbox FIFO;
 * the reactor applies HEADERS (streaming submit), CHUNKs (chunk_queue),
 * END (trailers + EOF). Backpressure credits are the step-3 follow-up —
 * until then append never reports BACKPRESSURE.
 * ------------------------------------------------------------------- */
/* Suspend the producer coroutine for `ms` on a one-shot timer so the worker
 * loop keeps draining while we wait for credit. Best-effort: bails silently
 * outside a coroutine (flush stays best-effort, mirroring the local path). */
static void worker_credit_sleep_ms(zend_coroutine_t *co, const zend_ulong ms)
{
    zend_async_timer_event_t *const t = ZEND_ASYNC_NEW_TIMER_EVENT(ms, false);

    if (UNEXPECTED(t == NULL)) {
        zend_clear_exception();
        return;
    }

    t->base.start(&t->base);
    zend_async_resume_when(co, &t->base, true, zend_async_waker_callback_resolve, NULL);
    ZEND_ASYNC_SUSPEND();
    ZEND_ASYNC_WAKER_DESTROY(co);
}

/* Park the producer until in-flight drops under the cap, the stream dies,
 * or the write timeout elapses. Returns false when the stream is dead. */
static bool worker_stream_wait_credit(worker_dispatch_ctx_t *ctx)
{
    if (ctx->credit == NULL) {
        return true;
    }

    const uint32_t timeout_ms =
        http_server_get_write_timeout_s(ctx->server) * 1000u;
    uint64_t waited_ms = 0;

    while (ctx->posted_bytes - stream_credit_acked(ctx->credit)
               >= WORKER_STREAM_INFLIGHT_CAP) {
        if (stream_credit_is_dead(ctx->credit)) {
            return false;
        }

        zend_coroutine_t *const co = ZEND_ASYNC_CURRENT_COROUTINE;

        if (co == NULL || ZEND_ASYNC_IS_SCHEDULER_CONTEXT) {
            return true;   /* can't suspend — degrade to unbounded */
        }

        worker_credit_sleep_ms(co, WORKER_CREDIT_POLL_MS);

        if (EG(exception) != NULL) {
            return false;   /* cancelled while parked */
        }

        waited_ms += WORKER_CREDIT_POLL_MS;

        if (timeout_ms > 0 && waited_ms >= timeout_ms) {
            return false;   /* peer stopped reading — treat as dead */
        }
    }

    return true;
}

static int worker_stream_append_chunk(void *vctx, zend_string *chunk)
{
    worker_dispatch_ctx_t *const ctx = (worker_dispatch_ctx_t *)vctx;

    if (UNEXPECTED(ctx->stream_ended)
        || (ctx->credit != NULL && stream_credit_is_dead(ctx->credit))) {
        zend_string_release(chunk);
        return HTTP_STREAM_APPEND_STREAM_DEAD;
    }

    /* First send(): the response just committed — flatten status + headers
     * into the STREAM_HEADERS wire that opens the stream on the reactor,
     * carrying the shared credit block (the reactor adopts one ref). */
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
        worker_wire_post(ctx, hw);
        ctx->stream_started = true;
    }

    response_wire_t *const cw =
        response_wire_create(ctx->reactor_id, ctx->stream_id, ctx->conn);

    if (UNEXPECTED(cw == NULL)) {
        zend_string_release(chunk);
        return HTTP_STREAM_APPEND_STREAM_DEAD;
    }

    response_wire_set_kind(cw, RESPONSE_WIRE_STREAM_CHUNK);

    if (UNEXPECTED(!response_wire_set_body(cw, ZSTR_VAL(chunk), ZSTR_LEN(chunk),
                                           false))) {
        response_wire_free(cw);
        zend_string_release(chunk);
        return HTTP_STREAM_APPEND_STREAM_DEAD;
    }

    const size_t chunk_len = ZSTR_LEN(chunk);

    worker_wire_post(ctx, cw);
    zend_string_release(chunk);   /* bytes copied into the wire arena */

    /* Flow control: account the bytes, then park until the reactor retires
     * enough of the backlog (peer ACKs) — mirrors the local path suspending
     * on write_event, but over the thread boundary via the credit block. */
    ctx->posted_bytes += chunk_len;

    if (!worker_stream_wait_credit(ctx)) {
        return HTTP_STREAM_APPEND_STREAM_DEAD;
    }

    return HTTP_STREAM_APPEND_OK;
}

/* Advisory for HttpResponse::sendable(): true while append_chunk would not
 * park (room under the in-flight cap and the stream is alive). */
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

    response_wire_set_kind(ew, RESPONSE_WIRE_STREAM_END);
    worker_wire_copy_trailers(ew, Z_OBJ(ctx->response_zv));
    worker_wire_post(ctx, ew);
}

static const http_response_stream_ops_t worker_stream_ops = {
    .append_chunk   = worker_stream_append_chunk,
    .sendable       = worker_stream_sendable,
    .mark_ended     = worker_stream_mark_ended,
    .get_wait_event = NULL,   /* backpressure parks inside append_chunk */
};

/* gRPC finish ops (grpc_call_finish seam) — the worker-side delivery
 * actions; what to send is decided in src/grpc/grpc_call.c. */

/* grpc-web in-body trailer frame. Streamed reply: one more CHUNK + END.
 * Zero-message reply: the frame becomes the buffered body — the FULL wire
 * rendered right after grpc_call_finish carries it. Consumes the ref. */
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

/* Trailers-Only: grpc_call_finish already promoted the trailers into the
 * initial HEADERS; dispose renders + posts the FULL wire right after this
 * returns, so there is nothing left to commit here. */
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
            response_wire_set_body(rw, ZSTR_VAL(body), ZSTR_LEN(body), true);
        } else {
            response_wire_set_body(rw, NULL, 0, true);
        }
    } else {
        response_wire_set_body(rw, NULL, 0, true);
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

    if (!Z_ISUNDEF(ctx->response_zv)) {
        zend_object *const resp = Z_OBJ(ctx->response_zv);

        /* A gRPC outcome is expressed as grpc-status on a 200, not as an
         * HTTP 500 — grpc_call_ensure_status maps the exception below. */
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

        /* Delivery shape. gRPC policy decides via grpc_call_finish; a plain
         * streamed response just needs its END (idempotent — a handler that
         * called end() already posted it). A response that never streamed is
         * the buffered FULL wire, exactly as before. */
        if (ctx->is_grpc) {
            grpc_call_finish(resp, ctx->grpc_web, &worker_grpc_finish_ops, ctx);
        } else if (http_response_is_streaming(resp)) {
            worker_stream_mark_ended(ctx);
        }

        if (!ctx->stream_started) {
            response_wire_t *const rw = worker_render_response(ctx);

            if (rw != NULL) {
                worker_wire_post(ctx, rw);   /* sink owns rw now */
            }
        }

        /* Detach the ops: a handler may have kept $response alive beyond
         * this dispose; ctx dies below, so a late send() must throw the
         * standard "not available" instead of touching freed memory. */
        http_response_replace_stream_ops(resp, NULL, NULL);
    }

    if (ctx->credit != NULL) {
        stream_credit_release(ctx->credit);   /* worker-side ref */
        ctx->credit = NULL;
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

    /* gRPC classification — once, here, exactly like the transports do
     * (application/grpc* + a registered addGrpcHandler). */
    HashTable *const handlers = http_server_get_protocol_handlers(server);
    const bool is_grpc  = grpc_request_is_grpc(req)
                          && http_protocol_has_handler(handlers, HTTP_PROTOCOL_GRPC);
    const bool grpc_web = is_grpc && grpc_request_is_grpc_web(req);

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
    ctx->grpc_web   = grpc_web;

    ZVAL_COPY_VALUE(&ctx->request_zv, req_obj);
    efree(req_obj);                 /* the heap zval wrapper, not the object */

    object_init_ex(&ctx->response_zv, http_response_ce);
    http_response_set_protocol_version(Z_OBJ(ctx->response_zv), "3.0");

    /* Streaming reverse path: send()/writeMessage() flatten into STREAM_*
     * wires posted through the sink instead of throwing. */
    http_response_install_stream_ops(Z_OBJ(ctx->response_zv),
                                     &worker_stream_ops, ctx);

    if (is_grpc) {
        grpc_call_init_response(Z_OBJ(ctx->response_zv), grpc_web);
    }

    /* No handler registered: synthesise a 404 so the sink still fires with a
     * response instead of leaving the stream hanging. */
    zend_fcall_t *fcall = is_grpc
        ? http_protocol_get_handler(handlers, HTTP_PROTOCOL_GRPC)
        : NULL;

    if (fcall == NULL) {
        fcall = http_protocol_get_handler(handlers, HTTP_PROTOCOL_HTTP1);
    }

    if (fcall == NULL) {
        fcall = http_protocol_get_handler(handlers, HTTP_PROTOCOL_HTTP2);
    }

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
