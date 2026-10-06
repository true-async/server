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
#include "core/response_delivery.h"
#include "core/async_plain_event.h"
#include "php_http_server.h"                 /* http_server_object accessors, response API */
#include "core/http_connection.h"            /* http_request_handler_coroutine_new */
#include "core/bailout_guard.h"
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
#include "fiu-local.h"
#ifdef HAVE_HTTP_COMPRESSION
#include "compression/http_compression_request.h"   /* request body decode */
#include "compression/http_compression_response.h"  /* response encode */
#endif

#define WORKER_STREAM_INFLIGHT_CAP (1024 * 1024)

#include <string.h>

/* Defined in src/http_request.c (no public header). Wraps an http_request_t in
 * an HttpRequest zval, taking ownership of the request's single reference. */
extern zval *http_request_create_from_parsed(http_request_t *req);

typedef struct worker_writer_waiter_s {
    struct worker_writer_waiter_s *volatile next;
    struct worker_writer_waiter_s *volatile prev;
    zend_async_event_t *volatile event;
    volatile bool probe;
} worker_writer_waiter_t;

/* Per-request worker-side dispatch state. Lives from worker_dispatch_request
 * until the handler and all active stream operations release their references;
 * ecalloc/efree on the worker thread. */
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

    /* The user handler died in a zend_bailout: the sender tail renders a 500.
     * Legacy handler telemetry is skipped; delivery accounting remains active. */
    bool                    handler_bailout;
    zend_object            *handler_exception;
    response_delivery_t    *delivery;
    bool                    sender_done;
    unsigned                refs;
    bool                    disposed;
    bool                    writer_busy;
    zend_coroutine_t       *writer_owner;
    unsigned                writer_depth;
    worker_writer_waiter_t  *writer_waiters;
    worker_writer_waiter_t  *writer_tail;
    worker_writer_waiter_t  *idle_waiters;
    worker_writer_waiter_t  *idle_tail;
    response_wire_t        *render_wire; /* isolated/test dispatch without a record */

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

    /* The transport the request arrived on; the handler pick reads it. */
    http_protocol_type_t    protocol;

    bool                    stream_started;
    bool                    stream_ended;
    bool                    stream_failed;  /* terminal wire becomes STREAM_ABORT */
    int64_t                 abort_code;     /* reset code; <0 = the reactor's own */

    stream_credit_t        *credit;         /* shared with the reactor */
    zend_async_trigger_event_t *credit_wake; /* worker-owned; reactor signals it */
    uint64_t                posted_bytes;
} worker_dispatch_ctx_t;
static const http_response_stream_ops_t worker_stream_ops;
static void worker_ctx_unref(worker_dispatch_ctx_t *ctx);

static void writer_remove(worker_dispatch_ctx_t *ctx, worker_writer_waiter_t *w)
{
    worker_writer_waiter_t **head = w->probe ? &ctx->idle_waiters : &ctx->writer_waiters;
    worker_writer_waiter_t **tail = w->probe ? &ctx->idle_tail : &ctx->writer_tail;
    if (w->prev != NULL) w->prev->next = w->next;
    else *head = w->next;
    if (w->next != NULL) w->next->prev = w->prev;
    else *tail = w->prev;
    if (!w->probe && !ctx->writer_busy && ctx->writer_waiters != NULL)
        async_plain_event_fire(ctx->writer_waiters->event);
    if (!w->probe && !ctx->writer_busy && ctx->writer_waiters == NULL)
        for (worker_writer_waiter_t *p = ctx->idle_waiters; p != NULL; p = p->next)
            async_plain_event_fire(p->event);
}

/* The worker alone owns this FIFO. No second coroutine may open HEADERS or
 * pass an END while the preceding sender is parked on mailbox capacity. */
static bool writer_enter_until(worker_dispatch_ctx_t *ctx, bool nonblocking,
                                uint64_t deadline, bool acquire)
{
    zend_coroutine_t *co = ZEND_ASYNC_CURRENT_COROUTINE;
    if (ctx->disposed) return false;
    if (ctx->writer_busy && ctx->writer_owner == co) {
        if (acquire) ctx->writer_depth++;
        return true;
    }
    if (!ctx->writer_busy && ctx->writer_waiters == NULL) {
        if (acquire) {
            ctx->writer_busy = true; ctx->writer_owner = co; ctx->writer_depth = 1;
        }
        return true;
    }
    if (nonblocking || co == NULL || ZEND_ASYNC_IS_SCHEDULER_CONTEXT) return false;
    worker_writer_waiter_t w = { .probe = !acquire };
    volatile bool linked = false, bailout = false, acquired = false;
    http_bailout_state_t state;
    http_bailout_state_save(&state);
    zend_try {
        w.event = async_plain_event_new();
        if (w.event != NULL) {
            worker_writer_waiter_t *tail = acquire ? ctx->writer_tail : ctx->idle_tail;
            w.prev = tail;
            if (tail == NULL) {
                if (acquire) ctx->writer_waiters = &w;
                else ctx->idle_waiters = &w;
            }
            else {
                tail->next = &w;
            }
            if (acquire) ctx->writer_tail = &w;
            else ctx->idle_tail = &w;
            linked = true;
            for (;;) {
                if (ctx->disposed || EG(exception) != NULL) break;
                if (deadline != 0 && zend_hrtime() >= deadline) break;
                if (!ctx->writer_busy && (acquire ? ctx->writer_waiters == &w
                                                  : ctx->writer_waiters == NULL)) {
                    if (acquire) {
                        ctx->writer_busy = true; ctx->writer_owner = co; ctx->writer_depth = 1;
                    }
                    acquired = true;
                    break;
                }
                ZEND_ASYNC_WAKER_NEW(co);
                zend_async_resume_when(co, w.event, false,
                                       zend_async_waker_callback_resolve, NULL);
                if (deadline != 0) {
                    uint64_t now = zend_hrtime();
                    zend_async_timer_event_t *timer = ZEND_ASYNC_NEW_TIMER_EVENT(
                        now < deadline ? (deadline - now + 999999) / 1000000 : 1, false);
                    if (timer == NULL) break;
                    zend_async_resume_when(co, &timer->base, true,
                                           zend_async_waker_callback_timeout, NULL);
                }
                if (ctx->writer_busy || (acquire ? ctx->writer_waiters != &w
                                                 : ctx->writer_waiters != NULL)) ZEND_ASYNC_SUSPEND();
                ZEND_ASYNC_WAKER_DESTROY(co);
            }
        }
    } zend_catch {
        http_bailout_state_restore(&state);
        bailout = true;
    } zend_end_try();
    ZEND_ASYNC_WAKER_DESTROY(co);
    if (linked) writer_remove(ctx, &w);
    if (w.event != NULL) w.event->dispose(w.event);
    if (bailout) zend_bailout();
    return acquired;
}

static bool writer_enter(worker_dispatch_ctx_t *ctx, bool nonblocking)
{
    return writer_enter_until(ctx, nonblocking, 0, true);
}

static void writer_leave(worker_dispatch_ctx_t *ctx)
{
    ZEND_ASSERT(ctx->writer_busy && ctx->writer_depth != 0);
    if (--ctx->writer_depth != 0) return;
    ctx->writer_busy = false; ctx->writer_owner = NULL;
    if (ctx->writer_waiters != NULL) async_plain_event_fire(ctx->writer_waiters->event);
    else for (worker_writer_waiter_t *w = ctx->idle_waiters; w != NULL; w = w->next)
        async_plain_event_fire(w->event);
}

static response_wire_t *worker_wire_create(worker_dispatch_ctx_t *ctx)
{
    response_wire_t *rw = response_wire_create(ctx->reactor_id, ctx->stream_id, ctx->conn);
    if (rw != NULL) {
        if (ctx->delivery != NULL) response_delivery_own_wire(ctx->delivery, rw);
        else ctx->render_wire = rw;
    }
    return rw;
}

static void worker_discard_owned(worker_dispatch_ctx_t *ctx)
{
    if (ctx->delivery != NULL) response_delivery_discard_owned(ctx->delivery);
    if (ctx->render_wire != NULL) {
        response_wire_discard(ctx->render_wire);
        ctx->render_wire = NULL;
    }
}

static void worker_encoded_offer_refused(worker_dispatch_ctx_t *ctx)
{
    if (ctx->delivery != NULL && !ctx->stream_failed
        && http_response_from_obj(Z_OBJ(ctx->response_zv))->stream_ops != &worker_stream_ops) {
        /* The codec has consumed input. A later queue refusal cannot be
         * retried from that encoder state or rendered as an empty gzip FULL. */
        ctx->stream_failed = true;
        response_delivery_cancel(ctx->delivery, "codec_post_failed");
        response_delivery_note_drop(ctx->delivery);
    }
}

#ifdef HAVE_HTTP_COMPRESSION
/* Decodes a Content-Encoding request body in place before the handler, as the
 * transports do. False when the body cannot be decoded: the response then
 * carries the status the decoder names (415, 413 or 400) and a plain-text
 * reason, the request carries its service window for the access record, and
 * the caller must not run the handler. */
static bool worker_decode_request_body(worker_dispatch_ctx_t *ctx)
{
    ZEND_ASSERT(!Z_ISUNDEF(ctx->request_zv));

    http_request_t *const req = http_request_from_zobj(Z_OBJ(ctx->request_zv));
    const int status =
        http_compression_decode_request_body(req, http_server_get_config(ctx->server));

    if (status == HTTP_DECODE_OK) {
        return true;
    }

    http_response_set_error(Z_OBJ(ctx->response_zv), status,
                            http_compression_decode_status_text(status));

    if (ctx->stamps) {
        req->start_ns = ctx->start_ns;
        req->end_ns   = zend_hrtime();
    }

    return false;
}
#endif

/* Handler coroutine body: run the registered user handler with (request, response). */
static void worker_dispatch_handler(void)
{
    const zend_coroutine_t *const co = ZEND_ASYNC_CURRENT_COROUTINE;
    worker_dispatch_ctx_t *const ctx = (worker_dispatch_ctx_t *)co->extended_data;
    ZEND_ASSERT(ctx != NULL);

    /* Synthetic 404 (no handler): the sender tail sends it like any other response. */
    if (ctx->skip_handler) {
        return;
    }

    HashTable *const handlers = http_server_get_protocol_handlers(ctx->server);
    zend_fcall_t *const fcall =
        http_protocol_pick_handler(handlers, ctx->protocol, ctx->is_grpc);

    if (fcall == NULL) {
        return;
    }

    if (ctx->stamps) {
        ctx->start_ns = zend_hrtime();
    }

#ifdef HAVE_HTTP_COMPRESSION
    if (!worker_decode_request_body(ctx)) {
        return;
    }
#endif

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
    http_bailout_state_t bailout_state;
    http_bailout_state_save(&bailout_state);

    zend_try {
        zend_call_function(&fci, &fcall->fci_cache);
    } zend_catch {
        http_bailout_state_restore(&bailout_state);
        bailout = true;
    } zend_end_try();

    if (UNEXPECTED(bailout)) {
        ctx->handler_bailout = true;
        return;
    }

    /* Stamp end before the retval dtor so destructor time is not charged as
     * service time. */
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

    zval_ptr_dtor(&retval);
}

/* Flatten status + H2/H3-allowed headers of the response onto a wire.
 *
 * @p cl_len digits of @p cl are added as the response's own count, and @p
 * keep_content_length says whether the table's field survives the copy. The
 * reactor submits the wire as it stands, so a name dropped here cannot be put
 * back downstream. */
static bool worker_wire_copy_head(response_wire_t *rw, zend_object *resp,
                                  const char *cl, const size_t cl_len,
                                  const bool keep_content_length)
{
    int status = http_response_get_status(resp);

    if (UNEXPECTED(status <= 0)) {
        status = 200;
    }

    response_wire_set_status(rw, status);

    if (cl_len != 0) {
        if (!response_wire_add_header(rw, "content-length", sizeof("content-length") - 1,
                                 cl, cl_len)) return false;
    }

    HashTable *const headers = http_response_get_headers(resp);

    if (headers == NULL) {
        return true;
    }

    zend_string *name;
    zval        *values;
    ZEND_HASH_FOREACH_STR_KEY_VAL(headers, name, values) {
        if (UNEXPECTED(name == NULL)) {
            continue;
        }

        if (!http_response_header_allowed_h2h3(ZSTR_VAL(name), ZSTR_LEN(name),
                                               keep_content_length)) {
            continue;
        }

        if (EXPECTED(Z_TYPE_P(values) == IS_STRING)) {
            if (!response_wire_add_header(rw, ZSTR_VAL(name), ZSTR_LEN(name),
                                     Z_STRVAL_P(values), Z_STRLEN_P(values))) return false;
        } else if (Z_TYPE_P(values) == IS_ARRAY) {
            zval *v;
            ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(values), v) {
                if (Z_TYPE_P(v) != IS_STRING) {
                    continue;
                }

                if (!response_wire_add_header(rw, ZSTR_VAL(name), ZSTR_LEN(name),
                                         Z_STRVAL_P(v), Z_STRLEN_P(v))) return false;
            } ZEND_HASH_FOREACH_END();
        }
    } ZEND_HASH_FOREACH_END();
    return true;
}

/* Flatten the response trailer map onto the wire. */
static bool worker_wire_copy_trailers(response_wire_t *rw, zend_object *resp)
{
    HashTable *const trailers = http_response_get_trailers(resp);

    if (trailers == NULL) {
        return true;
    }

    zend_string *name;
    zval        *val;
    ZEND_HASH_FOREACH_STR_KEY_VAL(trailers, name, val) {
        if (UNEXPECTED(name == NULL) || Z_TYPE_P(val) != IS_STRING) {
            continue;
        }

        if (!response_wire_add_trailer(rw, ZSTR_VAL(name), ZSTR_LEN(name),
                                  Z_STRVAL_P(val), Z_STRLEN_P(val))) return false;
    } ZEND_HASH_FOREACH_END();
    return true;
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
static bool worker_wire_post(worker_dispatch_ctx_t *ctx, response_wire_t *rw,
                             bool nonblocking)
{
    if (ctx->delivery != NULL) {
        if (response_delivery_post(ctx->delivery, rw,
                                    http_server_get_write_timeout_s(ctx->server) * 1000u,
                                    nonblocking)) return true;
        if (!response_delivery_failed(ctx->delivery)) return false;
        ctx->stream_failed = true;
        return false;
    }
    if (ctx->render_wire == rw) ctx->render_wire = NULL;
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
 * reactor signals per ack. @p bound_ms 0 takes the configured write deadline,
 * which is what bounds a peer that stops ACKing. */
static bool worker_stream_wait_credit(worker_dispatch_ctx_t *ctx,
                                      const uint32_t bound_ms, uint64_t deadline)
{
    if (ctx->disposed) return false;
    if (ctx->credit == NULL) {
        return true;
    }

    const uint32_t timeout_ms = bound_ms != 0
        ? bound_ms
        : http_server_get_write_timeout_s(ctx->server) * 1000u;
    if (deadline == 0 && timeout_ms != 0)
        deadline = zend_hrtime() + (uint64_t)timeout_ms * 1000000;

    while (ctx->posted_bytes - stream_credit_acked(ctx->credit)
               >= WORKER_STREAM_INFLIGHT_CAP) {
        if (deadline != 0 && zend_hrtime() >= deadline) return false;
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

        if (deadline != 0) {
            uint64_t now = zend_hrtime();
            zend_async_timer_event_t *timer = ZEND_ASYNC_NEW_TIMER_EVENT(
                now < deadline ? (deadline - now + 999999) / 1000000 : 1, false);
            if (timer == NULL) return false;
            zend_async_resume_when(co, &timer->base, true,
                                   zend_async_waker_callback_timeout, NULL);
        }

        ZEND_ASYNC_SUSPEND();
        zend_async_waker_clean(co);

        if (ctx->disposed || ctx->credit == NULL) return false;

        if (EG(exception) != NULL) {
            return false;   /* write timeout or cancelled while parked */
        }
    }

    return true;
}

static int worker_stream_append_impl(worker_dispatch_ctx_t *ctx, zend_string *chunk,
                                      const bool nonblocking, volatile bool *owned)
{

    if (UNEXPECTED(ctx->stream_ended || ctx->stream_failed)
        || (ctx->credit != NULL && stream_credit_is_dead(ctx->credit))) {
        *owned = false; zend_string_release(chunk);
        return HTTP_STREAM_APPEND_STREAM_DEAD;
    }

    /* Refused on the depth already in flight, letting this chunk overshoot the
     * cap — the rule H2 applies too. Counting the candidate's length instead
     * would refuse a chunk larger than the cap for ever, whatever the peer
     * did, and the caller would spin on it. */
    if (nonblocking && ctx->credit != NULL
        && ctx->posted_bytes - stream_credit_acked(ctx->credit)
               >= WORKER_STREAM_INFLIGHT_CAP) {
        *owned = false; zend_string_release(chunk);
        return HTTP_STREAM_APPEND_BACKPRESSURE;
    }

    /* first write(): open the stream; the reactor adopts one credit ref */
    if (!ctx->stream_started) {
        response_wire_t *const hw =
            worker_wire_create(ctx);

    if (UNEXPECTED(hw == NULL)) {
            if (ctx->delivery != NULL) response_delivery_cancel(ctx->delivery, "render_failed");
            *owned = false; zend_string_release(chunk);
            return HTTP_STREAM_APPEND_STREAM_DEAD;
        }

        ctx->credit = stream_credit_create();   /* NULL degrades to unbounded */

        response_wire_set_kind(hw, RESPONSE_WIRE_STREAM_HEADERS);
        response_wire_set_credit(hw, ctx->credit);

        if (!worker_wire_copy_head(hw, Z_OBJ(ctx->response_zv), NULL, 0,
            http_response_keeps_declared_length(Z_OBJ(ctx->response_zv)))) {
            worker_discard_owned(ctx);
            ctx->stream_failed = true;
            if (ctx->delivery != NULL) response_delivery_cancel(ctx->delivery, "render_failed");
            *owned = false; zend_string_release(chunk);
            return HTTP_STREAM_APPEND_STREAM_DEAD;
        }

        /* headers undeliverable → the stream never opened; don't copy and
         * post a chunk wire the reactor would only throw away */
        if (UNEXPECTED(!worker_wire_post(ctx, hw, nonblocking))) {
            if (nonblocking) worker_encoded_offer_refused(ctx);
            if (!ctx->stream_failed) {
                stream_credit_release(ctx->credit);
                ctx->credit = NULL;
            }
            *owned = false; zend_string_release(chunk);
            return ctx->stream_failed ? HTTP_STREAM_APPEND_STREAM_DEAD
                                      : HTTP_STREAM_APPEND_BACKPRESSURE;
        }

        /* Set only once the headers wire is away: this flag is what the
         * terminal wires read as "the reactor has a stream to end", and an
         * abort posted against a stream that never opened is applied to
         * nothing. */
        ctx->stream_started = true;
    }

    response_wire_t *const cw =
        worker_wire_create(ctx);

    if (UNEXPECTED(cw == NULL)) {
        if (ctx->delivery != NULL) response_delivery_cancel(ctx->delivery, "render_failed");
        *owned = false; zend_string_release(chunk);
        return HTTP_STREAM_APPEND_STREAM_DEAD;
    }

    response_wire_set_kind(cw, RESPONSE_WIRE_STREAM_CHUNK);

    /* one copy: ZMM -> persistent; the reactor adopts the ref into its ring */
    zend_string *const pchunk =
        zend_string_init(ZSTR_VAL(chunk), ZSTR_LEN(chunk), 1);

    response_wire_set_chunk(cw, pchunk);

    const size_t chunk_len = ZSTR_LEN(chunk);

    /* A refused wire is a dropped chunk: the sink exhausted its retries and
     * worker_wire_post has already marked the stream failed. Reporting OK here
     * would tell the handler it wrote bytes the peer will never see. */
    if (UNEXPECTED(!worker_wire_post(ctx, cw, nonblocking))) {
        if (nonblocking) worker_encoded_offer_refused(ctx);
        *owned = false; zend_string_release(chunk);
        return ctx->stream_failed ? HTTP_STREAM_APPEND_STREAM_DEAD
                                  : HTTP_STREAM_APPEND_BACKPRESSURE;
    }

    *owned = false; zend_string_release(chunk);   /* bytes copied into the wire arena */

    ctx->posted_bytes += chunk_len;

    if (nonblocking) {
        return HTTP_STREAM_APPEND_OK;   /* room was checked above; never parks */
    }

    if (ctx->disposed) return HTTP_STREAM_APPEND_STREAM_DEAD;
    if (!worker_stream_wait_credit(ctx, 0, 0)) {
        ctx->stream_failed = true;   /* credit timeout / cancelled while parked */
        return HTTP_STREAM_APPEND_STREAM_DEAD;
    }

    return HTTP_STREAM_APPEND_OK;
}

static int worker_stream_append_chunk(void *arg, zend_string *chunk, bool nonblocking)
{
    worker_dispatch_ctx_t *ctx = arg;
    ctx->refs++;
    volatile bool locked = false, owned = true, bailout = false;
    volatile int rc = HTTP_STREAM_APPEND_STREAM_DEAD;
    http_bailout_state_t state;
    http_bailout_state_save(&state);
    zend_try {
        locked = writer_enter(ctx, nonblocking);
        if (locked) rc = worker_stream_append_impl(ctx, chunk, nonblocking, &owned);
        else if (!ctx->disposed && !ctx->stream_failed && nonblocking)
            rc = HTTP_STREAM_APPEND_BACKPRESSURE;
    } zend_catch {
        http_bailout_state_restore(&state);
        bailout = true;
        worker_discard_owned(ctx);
        ctx->stream_failed = true;
        if (ctx->delivery != NULL) response_delivery_cancel(ctx->delivery, "render_failed");
    } zend_end_try();
    if (owned) zend_string_release(chunk);
    if (locked) writer_leave(ctx);
    worker_ctx_unref(ctx);
    if (bailout) zend_bailout();
    return rc;
}

/* sendable() advisory: true while append_chunk would not park. */
static bool worker_stream_sendable(void *vctx)
{
    worker_dispatch_ctx_t *const ctx = (worker_dispatch_ctx_t *)vctx;

    if (ctx->disposed || ctx->stream_ended || ctx->stream_failed
        || (ctx->writer_busy && ctx->writer_owner != ZEND_ASYNC_CURRENT_COROUTINE)
        || ctx->writer_waiters != NULL) {
        return false;
    }

    if (ctx->delivery != NULL && !response_delivery_sendable(ctx->delivery)) return false;

    if (ctx->credit == NULL) {
        return true;
    }

    return !stream_credit_is_dead(ctx->credit)
           && ctx->posted_bytes - stream_credit_acked(ctx->credit)
                  < WORKER_STREAM_INFLIGHT_CAP;
}

/* The reactor side kills the credit when the stream dies, and stream_failed
 * records a credit wait this worker already lost. */
static bool worker_stream_is_alive(void *vctx)
{
    const worker_dispatch_ctx_t *const ctx = (const worker_dispatch_ctx_t *)vctx;

    if (ctx->disposed || ctx->stream_ended || ctx->stream_failed) {
        return false;
    }

    return ctx->credit == NULL || !stream_credit_is_dead(ctx->credit);
}

static void worker_stream_mark_ended_impl(worker_dispatch_ctx_t *ctx)
{

    if (!ctx->stream_started || ctx->stream_ended) {
        return;
    }

    ctx->stream_ended = true;

    response_wire_t *const ew =
        worker_wire_create(ctx);

    if (UNEXPECTED(ew == NULL)) {
        if (ctx->delivery != NULL) response_delivery_cancel(ctx->delivery, "render_failed");
        return;
    }

    if (ctx->stream_failed) {
        response_wire_set_kind(ew, RESPONSE_WIRE_STREAM_ABORT);
        response_wire_set_abort_code(ew, ctx->abort_code);
    } else {
        response_wire_set_kind(ew, RESPONSE_WIRE_STREAM_END);
        if (!worker_wire_copy_trailers(ew, Z_OBJ(ctx->response_zv))) {
            worker_discard_owned(ctx);
            if (ctx->delivery != NULL) response_delivery_cancel(ctx->delivery, "render_failed");
            return;
        }
    }

    worker_wire_post(ctx, ew, false);
}

static void worker_stream_mark_ended(void *arg)
{
    worker_dispatch_ctx_t *ctx = arg;
    ctx->refs++;
    volatile bool locked = false, bailout = false;
    http_bailout_state_t state;
    http_bailout_state_save(&state);
    zend_try {
        locked = writer_enter(ctx, false);
        if (locked) worker_stream_mark_ended_impl(ctx);
    } zend_catch {
        http_bailout_state_restore(&state);
        bailout = true;
        worker_discard_owned(ctx);
        if (ctx->delivery != NULL) response_delivery_cancel(ctx->delivery, "render_failed");
    } zend_end_try();
    if (locked) writer_leave(ctx);
    worker_ctx_unref(ctx);
    if (bailout) zend_bailout();
}

/* The terminal wire above is already the abort one when stream_failed is set,
 * and the reactor answers that kind with a stream reset — so failing a stream
 * from this side is one flag plus the finisher that reads it. */
static bool worker_stream_abort(void *vctx, const int64_t error_code)
{
    worker_dispatch_ctx_t *const ctx = (worker_dispatch_ctx_t *)vctx;

    /* No headers wire has been posted, so the reactor has no stream to reset
     * and would drop the abort on the floor. Report it, and let the caller
     * take the clean finish that commits an empty response instead. */
    if (!ctx->stream_started) {
        return false;
    }

    ctx->stream_failed = true;
    ctx->abort_code    = error_code;
    worker_stream_mark_ended(ctx);
    return true;
}

/* The credit wait the blocking path takes, offered to a non-blocking caller
 * that asked to be told when room comes back. */
static uint32_t writer_remaining_ms(uint64_t deadline)
{
    if (deadline == 0) return 0;
    uint64_t now = zend_hrtime();
    return now < deadline ? (uint32_t)((deadline - now + 999999) / 1000000) : 1;
}

static bool worker_stream_wait_writable_impl(worker_dispatch_ctx_t *ctx, uint64_t deadline)
{
    if (ctx->disposed) return false;

    if (ctx->delivery != NULL && !response_delivery_wait_writable(ctx->delivery,
        writer_remaining_ms(deadline)))
        return false;

    if (deadline != 0 && zend_hrtime() >= deadline) return false;
    return worker_stream_wait_credit(ctx, writer_remaining_ms(deadline), deadline);
}

static bool worker_stream_wait_writable(void *arg, uint32_t timeout_ms)
{
    worker_dispatch_ctx_t *ctx = arg;
    ctx->refs++;
    volatile bool result = false, bailout = false;
    uint32_t bound = ctx->disposed ? 0 : (timeout_ms != 0 ? timeout_ms
        : http_server_get_write_timeout_s(ctx->server) * 1000u);
    uint64_t deadline = bound != 0 ? zend_hrtime() + (uint64_t)bound * 1000000 : 0;
    http_bailout_state_t state;
    http_bailout_state_save(&state);
    zend_try {
        while (!ctx->disposed && !ctx->stream_ended && !ctx->stream_failed) {
            /* Readiness waits do not reserve the writer between API calls:
             * otherwise two tryWrite/awaitWritable loops can pass a lock back
             * and forth forever without either offering its chunk. */
            if (!writer_enter_until(ctx, false, deadline, false)) break;
            if (!worker_stream_wait_writable_impl(ctx, deadline)) break;
            if (worker_stream_sendable(ctx)) { result = true; break; }
            if (deadline != 0 && zend_hrtime() >= deadline) break;
        }
    }
    zend_catch {
        http_bailout_state_restore(&state);
        bailout = true;
        if (ctx->delivery != NULL) response_delivery_cancel(ctx->delivery, "wait_failed");
    } zend_end_try();
    worker_ctx_unref(ctx);
    if (bailout) zend_bailout();
    return result;
}

static bool worker_stream_is_started(void *vctx)
{
    worker_dispatch_ctx_t *ctx = vctx;
    return ctx->stream_started || ctx->writer_busy || ctx->writer_waiters != NULL;
}

static const http_response_stream_ops_t worker_stream_ops = {
    .append_chunk   = worker_stream_append_chunk,
    .sendable       = worker_stream_sendable,
    .is_alive       = worker_stream_is_alive,
    .wait_writable  = worker_stream_wait_writable,
    .mark_ended     = worker_stream_mark_ended,
    .abort          = worker_stream_abort,
    .get_wait_event = NULL,   /* the wait above is the one to take */
    .is_started     = worker_stream_is_started,
};

/* grpc-web in-body trailer frame; consumes the ref. */
static void worker_grpc_append_frame_and_end(void *vctx, zend_string *frame)
{
    worker_dispatch_ctx_t *const ctx = (worker_dispatch_ctx_t *)vctx;

    if (http_response_is_streaming(Z_OBJ(ctx->response_zv))) {
        /* append_chunk consumes the ref (success or failure). */
        if (worker_stream_append_chunk(ctx, frame, false) == HTTP_STREAM_APPEND_OK) {
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

/* Trailers-Only: the sender tail posts the FULL wire after this returns. */
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
static response_wire_t *worker_render_response(worker_dispatch_ctx_t *ctx)
{
    fiu_return_on("h3/delivery/render_failed", NULL);
    zend_object *const resp = Z_OBJ(ctx->response_zv);

    response_wire_t *const rw =
        worker_wire_create(ctx);

    if (rw == NULL) {
        return NULL;
    }

#ifdef HAVE_HTTP_COMPRESSION
    /* Before the head and the length are read: encoding replaces the body and
     * sets Content-Encoding and Vary. */
    http_compression_apply_buffered(resp);
#endif

    char cl[HTTP_CONTENT_LENGTH_DIGITS];
    bool keep_cl;
    const size_t cl_len = http_response_wire_content_length(resp, cl, &keep_cl);

    if (!worker_wire_copy_head(rw, resp, cl, cl_len, keep_cl)
        || !worker_wire_copy_trailers(rw, resp)) {
        worker_discard_owned(ctx);
        return NULL;
    }

    /* http_response_get_body_str returns a borrowed reference; the bytes are
     * copied into a persistent wire-owned string, so nothing to release here.
     * HEAD carries the headers
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
/* Span of a request header, for the SEND_FILE wire. NULL when absent. */
static const char *worker_req_header(const http_request_t *req, const char *name,
                                     const size_t name_len, size_t *len_out)
{
    const zend_string *const v = req != NULL
        ? http_request_find_header(req, name, name_len) : NULL;

    *len_out = v != NULL ? ZSTR_LEN(v) : 0;
    return v != NULL ? ZSTR_VAL(v) : NULL;
}

static response_wire_t *worker_render_send_file(worker_dispatch_ctx_t *ctx,
                                                const http_send_file_request_t *sf)
{
    response_wire_t *const rw =
        worker_wire_create(ctx);

    if (rw == NULL) {
        return NULL;
    }

    response_wire_set_kind(rw, RESPONSE_WIRE_SEND_FILE);

    /* The engine runs on the reactor, which must not read this request: our
     * dispose frees its fields on this thread. Copy across what it needs. */
    const http_request_t *const req = !Z_ISUNDEF(ctx->request_zv)
        ? http_request_from_zobj(Z_OBJ(ctx->request_zv)) : NULL;

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

    if (req != NULL) {
        wsf.method   = req->method != NULL ? ZSTR_VAL(req->method) : NULL;
        wsf.method_len = req->method != NULL ? ZSTR_LEN(req->method) : 0;
        wsf.uri      = req->uri != NULL ? ZSTR_VAL(req->uri) : NULL;
        wsf.uri_len  = req->uri != NULL ? ZSTR_LEN(req->uri) : 0;
        wsf.range    = worker_req_header(req, "range", 5, &wsf.range_len);
        wsf.if_range = worker_req_header(req, "if-range", 8, &wsf.if_range_len);
        wsf.if_modified_since =
            worker_req_header(req, "if-modified-since", 17, &wsf.if_modified_since_len);
        wsf.if_none_match =
            worker_req_header(req, "if-none-match", 13, &wsf.if_none_match_len);
        wsf.accept_encoding =
            worker_req_header(req, "accept-encoding", 15, &wsf.accept_encoding_len);
    }

    if (!response_wire_set_send_file(rw, &wsf)) {
        worker_discard_owned(ctx);
        return NULL;
    }

    return rw;
}

/* Coroutine sender tail: commit the response (or derive a 500 from an
 * unhandled exception), render a response_wire, and hand it to the sink.
 * This path may suspend; disposal releases the per-request state separately. */
static void worker_dispatch_finalize(worker_dispatch_ctx_t *ctx,
                                      zend_coroutine_t *coroutine)
{

    /* A thrown handler exception becomes a response (derived 500 below, or
     * a grpc-status / aborted stream) — mark it consumed on both escalation
     * paths so it isn't rethrown into EG and trip a premature graceful
     * shutdown of the worker (#101; see http_handler_coroutine_dispose). */
    if (coroutine->exception != NULL) {
        ZEND_COROUTINE_SET_EXCEPTION_HANDLED(coroutine);
        ZEND_ASYNC_EVENT_SET_EXC_CAUGHT(&coroutine->event);
    }

    bool marshalled_send_file = false;

    if (!Z_ISUNDEF(ctx->response_zv)) {
        zend_object *const resp = Z_OBJ(ctx->response_zv);

        /* gRPC maps exceptions to grpc-status, not HTTP 500 */
        if (ctx->handler_exception != NULL && !ctx->is_grpc
            && !http_response_is_committed(resp)) {
            http_response_reset_to_error(resp, 500, "Internal Server Error");
        }

        if (ctx->is_grpc) {
            grpc_call_ensure_status(resp,
                ctx->handler_exception != NULL || ctx->handler_bailout);
        } else {
            http_response_reset_after_bailout(resp, ctx->handler_bailout);
        }

        if (!http_response_is_committed(resp)) {
            http_response_set_committed(resp);
        }

        if (ctx->is_grpc) {
            grpc_call_finish(resp, &worker_grpc_finish_ops, ctx);
        } else if (http_response_is_streaming(resp)) {
            (void)http_response_finish_stream(
                resp, ctx->handler_exception != NULL || ctx->handler_bailout, -1);
        }

        /* a started stream must always get a terminal wire */
        if (ctx->stream_started && !ctx->stream_ended) {
            worker_stream_mark_ended(ctx);
        }

        if (!ctx->stream_started && !ctx->stream_failed
            && (ctx->delivery == NULL || !response_delivery_failed(ctx->delivery))) {
            /* sendFile() seals the response: marshal path + opts to the
             * reactor, which opens the file and runs the sendfile engine.
             * Falls through to the buffered render when absent (#105). */
            http_send_file_request_t *const sf =
                ctx->is_grpc ? NULL : http_response_take_send_file(resp);
            const bool is_send_file = sf != NULL;

            response_wire_t *rw;

            if (is_send_file) {
                rw = worker_render_send_file(ctx, sf);
                http_send_file_request_free(sf);
            } else {
                rw = worker_render_response(ctx);
            }

            if (rw != NULL) {
                /* Legacy sendFile accounting belongs to the reactor only after
                 * acceptance. A live delivery record reports its final outcome
                 * on the originating worker for every response kind. */
                marshalled_send_file =
                    worker_wire_post(ctx, rw, false) && is_send_file;   /* sink owns rw now */
            } else if (ctx->delivery != NULL) {
                response_delivery_cancel(ctx->delivery, "render_failed");
            }
        }

        /* Legacy telemetry without a delivery record: a marshalled sendFile
         * obtains its final status in reactor cleanup. Live records defer all
         * accounting until the transport outcome and sender snapshot are ready. */
        if (ctx->delivery == NULL && EXPECTED(!ctx->handler_bailout) && !marshalled_send_file) {
            http_request_telemetry(Z_ISUNDEF(ctx->request_zv)
                                     ? NULL
                                     : http_request_from_zobj(Z_OBJ(ctx->request_zv)),
                                 resp, ctx->counters,
                                 http_server_get_log_state(ctx->server));
        }

        /* Detach operations before cleanup: a late write() on a retained
         * response must throw rather than access a released context. */
        http_response_replace_stream_ops(resp, NULL, NULL);
    }
}

/* A coroutine tail, not a destructor: queue capacity may suspend this sender.
 * Every handler outcome, including synthetic responses, reaches this tail. */
static void worker_dispatch_entry(void)
{
    zend_coroutine_t *co = ZEND_ASYNC_CURRENT_COROUTINE;
    worker_dispatch_ctx_t *ctx = co->extended_data;
    worker_dispatch_handler();
    if (EG(exception) != NULL) {
        ctx->handler_exception = EG(exception);
        GC_ADDREF(ctx->handler_exception);
        zend_clear_exception();
    }
    http_bailout_state_t state;
    http_bailout_state_save(&state);
    volatile bool locked = false;
    zend_try {
        locked = writer_enter(ctx, false);
        if (locked) worker_dispatch_finalize(ctx, co);
        else if (ctx->delivery != NULL) response_delivery_cancel(ctx->delivery, "sender_cancelled");
    } zend_catch {
        http_bailout_state_restore(&state);
        ctx->handler_bailout = true;
        worker_discard_owned(ctx);
        if (ctx->delivery != NULL) response_delivery_cancel(ctx->delivery, "render_failed");
    } zend_end_try();
    if (locked) writer_leave(ctx);
    if (ctx->delivery != NULL) {
        response_delivery_sender_done(ctx->delivery,
            http_request_from_zobj(Z_OBJ(ctx->request_zv)), Z_OBJ(ctx->response_zv),
            true);
    }
    ctx->sender_done = true;
    if (EG(exception) != NULL) zend_clear_exception();
}

static void worker_dispatch_dispose(zend_coroutine_t *coroutine)
{
    worker_dispatch_ctx_t *ctx = coroutine->extended_data;
    coroutine->extended_data = NULL;
    http_server_on_request_dispose(ctx->counters);
    if (!ctx->sender_done && ctx->delivery != NULL) {
        response_delivery_cancel(ctx->delivery, "sender_cancelled");
        response_delivery_sender_done(ctx->delivery,
            http_request_from_zobj(Z_OBJ(ctx->request_zv)), Z_OBJ(ctx->response_zv),
            true);
    }
    if (coroutine->exception != NULL) {
        ZEND_COROUTINE_SET_EXCEPTION_HANDLED(coroutine);
        ZEND_ASYNC_EVENT_SET_EXC_CAUGHT(&coroutine->event);
    }
    ctx->disposed = true;
    if (!Z_ISUNDEF(ctx->response_zv))
        http_response_replace_stream_ops(Z_OBJ(ctx->response_zv), NULL, NULL);
    for (worker_writer_waiter_t *w = ctx->writer_waiters; w != NULL; w = w->next)
        async_plain_event_fire(w->event);
    for (worker_writer_waiter_t *w = ctx->idle_waiters; w != NULL; w = w->next)
        async_plain_event_fire(w->event);
    worker_discard_owned(ctx);
    if (ctx->handler_exception != NULL) OBJ_RELEASE(ctx->handler_exception);
    if (ctx->delivery != NULL) response_delivery_release(ctx->delivery);

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
        async_plain_event_fire(&ctx->credit_wake->base);
        ZEND_ASYNC_EVENT_SET_CLOSED(&ctx->credit_wake->base);
        ctx->credit_wake->base.dispose(&ctx->credit_wake->base);
        ctx->credit_wake = NULL;
    }

    worker_ctx_unref(ctx);
}

static void worker_ctx_unref(worker_dispatch_ctx_t *ctx)
{
    ZEND_ASSERT(ctx->refs != 0);
    if (--ctx->refs != 0) return;
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

void worker_dispatch_cancel_request(http_request_t *req)
{
    if (req == NULL) return;
    response_delivery_t *d = req->delivery;
    req->delivery = NULL;
    if (d != NULL) {
        response_delivery_worker_begin(d, req);
        response_delivery_cancel(d, "dispatch_failed");
        response_delivery_sender_done(d, req, NULL, true);
    }
    http_request_destroy(req);
}

bool worker_dispatch_request(http_server_object *server,
                             zend_async_scope_t *scope,
                             http_request_t *req,
                             const bool own_scope,
                             worker_response_sink_fn sink, void *sink_arg)
{
    if (UNEXPECTED(server == NULL || scope == NULL || req == NULL)) {
        if (req != NULL) {
            worker_dispatch_cancel_request(req);
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

    if (req->delivery != NULL) response_delivery_worker_begin(req->delivery, req);

    zval *const req_obj = http_request_create_from_parsed(req);

    if (UNEXPECTED(req_obj == NULL)) {
        worker_dispatch_cancel_request(req);
        return false;
    }

    worker_dispatch_ctx_t *const ctx = ecalloc(1, sizeof(*ctx));
    ctx->refs = 1;
    ctx->server     = server;
    ctx->delivery   = req->delivery;
    ctx->counters   = http_server_counters(server);
    ctx->stamps     = http_server_sample_stamps_enabled(http_server_view(server));
    ctx->reactor_id = reactor_id;
    ctx->stream_id  = stream_id;
    ctx->conn       = conn;
    ctx->sink       = sink;
    ctx->sink_arg   = sink_arg;
    ctx->is_head    = is_head;
    ctx->is_grpc    = is_grpc;
    ctx->protocol   = req->http_major >= 3 ? HTTP_PROTOCOL_HTTP3
                    : req->http_major == 2 ? HTTP_PROTOCOL_HTTP2
                                           : HTTP_PROTOCOL_HTTP1;
    ctx->abort_code = -1;   /* until abort($code) names one */

    ZVAL_COPY_VALUE(&ctx->request_zv, req_obj);
    efree(req_obj);                 /* the heap zval wrapper, not the object */

    /* The request's own version in the "major.minor" form HTTP/1 sets: "3.0" on HTTP/3. */
    const char version[] = {
        (char)('0' + req->http_major), '.', (char)('0' + req->http_minor), '\0'
    };

    object_init_ex(&ctx->response_zv, http_response_ce);
    http_response_set_protocol_version(Z_OBJ(ctx->response_zv), version);
    http_response_set_head(Z_OBJ(ctx->response_zv), is_head);

    http_response_install_stream_ops(Z_OBJ(ctx->response_zv),
                                     &worker_stream_ops, ctx);

    /* The config a worker clone loaded on its own thread from the frozen
     * snapshot, so the MIME whitelist compression reads belongs to this
     * thread. A streamed body is wrapped at its first write(); a buffered
     * one is encoded when worker_render_response flattens it. */
    http_server_config_t *const cfg = http_server_get_config(server);

    http_request_prepare_dispatch(req, Z_OBJ(ctx->response_zv), cfg,
                                  http_server_view(server)->telemetry_enabled);

    if (is_grpc) {
        grpc_call_init_response(Z_OBJ(ctx->response_zv), grpc_mode);
    }

    /* No handler registered: synthesise a 404 so the sink still fires with a
     * response instead of leaving the stream hanging. */
    zend_fcall_t *const fcall =
        http_protocol_pick_handler(handlers, ctx->protocol, is_grpc);

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
        if (ctx->delivery != NULL) {
            response_delivery_cancel(ctx->delivery, "spawn_failed");
            response_delivery_sender_done(ctx->delivery, req, Z_OBJ(ctx->response_zv), true);
        }
        zval_ptr_dtor(&ctx->request_zv);
        zval_ptr_dtor(&ctx->response_zv);
        efree(ctx);
        return false;
    }

    if (ctx->delivery != NULL) response_delivery_addref(ctx->delivery); /* coroutine ctx */
    req->delivery = NULL; /* ctx now owns the dispatch-failure path */

    /* Bracket the in-flight request on the worker's counters (++active),
     * paired with on_request_dispose at coroutine dispose. */
    http_server_on_request_dispatch(ctx->counters);

    if (ctx->stamps) {
        ctx->enqueue_ns = zend_hrtime();
    }

    ZEND_ASYNC_ENQUEUE_COROUTINE(co);

    return true;
}
