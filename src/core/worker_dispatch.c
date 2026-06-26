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
#include "Zend/zend_hrtime.h"                /* zend_hrtime — request-service sampling */

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
} worker_dispatch_ctx_t;

/* Handler coroutine body: run the registered user handler with (request, response). */
static void worker_dispatch_entry(void)
{
    const zend_coroutine_t *const co = ZEND_ASYNC_CURRENT_COROUTINE;
    worker_dispatch_ctx_t *const ctx = (worker_dispatch_ctx_t *)co->extended_data;

    if (ctx == NULL) {
        return;
    }

    /* Synthetic 404 (no handler) still counts as a served request. */
    if (ctx->skip_handler) {
        http_server_count_request(ctx->counters);
        return;
    }

    HashTable *const handlers = http_server_get_protocol_handlers(ctx->server);
    zend_fcall_t *fcall = http_protocol_get_handler(handlers, HTTP_PROTOCOL_HTTP1);

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

/* Flatten the committed HttpResponse into a response_wire. Buffered only.
 * Returns NULL on allocation failure. */
static response_wire_t *worker_render_response(worker_dispatch_ctx_t *ctx)
{
    zend_object *const resp = Z_OBJ(ctx->response_zv);

    response_wire_t *const rw =
        response_wire_create(ctx->reactor_id, ctx->stream_id, ctx->conn);

    if (rw == NULL) {
        return NULL;
    }

    int status = http_response_get_status(resp);

    if (status <= 0) {
        status = 200;
    }

    response_wire_set_status(rw, status);

    HashTable *const headers = http_response_get_headers(resp);

    if (headers != NULL) {
        zend_string *name;
        zval        *values;
        ZEND_HASH_FOREACH_STR_KEY_VAL(headers, name, values) {
            if (name == NULL) {
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

    if (ctx == NULL) {
        return;
    }

    coroutine->extended_data = NULL;

    /* Un-bracket the in-flight request (--active), paired with the
     * on_request_dispatch in worker_dispatch_request. */
    http_server_on_request_dispose(ctx->counters);

    if (!Z_ISUNDEF(ctx->response_zv)) {
        zend_object *const resp = Z_OBJ(ctx->response_zv);

        if (coroutine->exception != NULL && !http_response_is_committed(resp)) {
            http_response_reset_to_error(resp, 500, "Internal Server Error");
        }

        if (!http_response_is_committed(resp)) {
            http_response_set_committed(resp);
        }

        response_wire_t *const rw = worker_render_response(ctx);

        if (rw != NULL) {
            if (ctx->sink != NULL) {
                ctx->sink(rw, ctx->sink_arg);   /* sink owns rw now */
            } else {
                response_wire_free(rw);
            }
        }
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

    ZVAL_COPY_VALUE(&ctx->request_zv, req_obj);
    efree(req_obj);                 /* the heap zval wrapper, not the object */

    object_init_ex(&ctx->response_zv, http_response_ce);
    http_response_set_protocol_version(Z_OBJ(ctx->response_zv), "3.0");

    /* No handler registered: synthesise a 404 so the sink still fires with a
     * response instead of leaving the stream hanging. */
    HashTable *const handlers = http_server_get_protocol_handlers(server);
    zend_fcall_t *fcall = http_protocol_get_handler(handlers, HTTP_PROTOCOL_HTTP1);

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
