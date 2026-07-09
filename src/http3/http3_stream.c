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

#include <php.h>
#ifndef PHP_WIN32
# include <sys/mman.h>             /* munmap — hq-interop mmap'd file bodies (POSIX) */
#endif
#include "Zend/zend_async_API.h"   /* zend_async_trigger_event_t dispose */
#include "http3/http3_stream.h"
#include "http3/http3_stream_pool.h"
#include "http3_connection.h"   /* http3_connection_t — list ownership at teardown */
#include "core/stream_credit.h" /* reverse-path flow control — teardown release */
#include "http_body_stream.h"   /* sever body_h3_conn + wake a parked consumer */
#include "http3_listener.h"     /* http3_listener_stream_pool */

/* Static-delivery memory accounting (http3_static_response.c). Declared here
 * rather than via the heavy http3_internal.h — teardown only needs the debit. */
extern void h3_static_account_debit(size_t n);


static void http3_stream_release_via_request(http_request_t *req);

http3_stream_t *http3_stream_new(http3_connection_t *conn, int64_t stream_id)
{
    http3_stream_pool_t *pool = http3_listener_stream_pool(conn->listener);
    http3_stream_t *s = http3_stream_pool_alloc(pool);

    if (UNEXPECTED(s == NULL)) {
        return NULL;
    }

    s->stream_id = stream_id;
    s->refcount  = 1;
    s->pool      = pool;

    /* Wire up the embedded request. _request_storage was zeroed by the
     * pool memset (or ecalloc); just set the alias pointer and the
     * release callback so http_request_destroy returns this slot to
     * the pool when the request refcount hits zero. */
    s->request = &s->_request_storage;
    s->request->refcount = 1;
    s->request->release  = http3_stream_release_via_request;
    /* Reactor mode: the listener routes parsed requests to PHP
     * workers, so the parser builds the request in the persistent (malloc)
     * domain — it crosses the reactor->worker thread boundary. NULL reactor ctx
     * (the default) keeps the ZMM fast path. */
    s->reactor_owned = (http3_listener_reactor_ctx(conn->listener) != NULL);
    s->request->persistent = s->reactor_owned;
    /* PHP zvals start UNDEF; dispatch fills them right before spawning
     * the handler coroutine. */
    ZVAL_UNDEF(&s->request_zv);
    ZVAL_UNDEF(&s->response_zv);
    return s;
}

/* Release callback fires from http_request_destroy when the request
 * refcount finally reaches zero. By that point http3_stream_release
 * has already done every field-level teardown step on the stream
 * (smart_str / chunk_queue / zvals / unlink). All this hook does is
 * return the slot to the slab pool — separating these phases lets
 * the slot stay alive across the gap between stream_release (early)
 * and the eventual destroy from a PHP HttpRequest wrapper (late). */
/* Reverse-path consumed apply, run ON THE REACTOR thread: the
 * worker is done with the request, so drop the reactor's worker-borrow stream
 * ref. When it is the last ref, the slab slot returns to the pool here (all
 * slab ops stay on the reactor that owns the pool). */
static void http3_reactor_consumed_apply(void *arg)
{
    http3_stream_release((http3_stream_t *)arg);
}

static void http3_stream_release_via_request(http_request_t *req)
{
    /* Offset-0 invariant: _request_storage is the first field of
     * http3_stream_t, so the same byte address is both. */
    http3_stream_t *s = (http3_stream_t *)req;

    /* Reactor mode: this fires on the WORKER thread (the request's
     * last ref dropped as the HttpRequest wrapper was freed). The slab is the
     * reactor's — freeing it here would be a cross-thread pool free. Instead
     * signal the owning reactor to reclaim the slot on its own thread;
     * the actual http3_stream_pool_free happens in http3_reactor_consumed_apply
     * -> http3_stream_release. */
    const http3_reactor_ctx_t *const rctx =
        s->conn != NULL ? http3_listener_reactor_ctx(s->conn->listener) : NULL;

    if (rctx != NULL) {
        /* Hand the release back via the worker's ordered FIFO — no busy-spin,
         * stays behind any parked wire of this stream. The fallback only fires
         * when no timer can be armed (stopping loop), where the FIFO is empty. */
        if (!http_worker_reactor_post_release(rctx->reactor_id,
                                              http3_reactor_consumed_apply, s)) {
            while (!reactor_pool_post_exec(rctx->pool, rctx->reactor_id,
                                           http3_reactor_consumed_apply, s)) {
            }
        }

        return;
    }

    http3_stream_pool_free(s->pool, s);
}

void http3_stream_release(http3_stream_t *s)
{
    if (s == NULL) {
        return;
    }

    if (--s->refcount > 0) {
        return;
    }

    /* Reactor mode: the worker owns the request's lifetime; the slab slot is
     * ours to reclaim. Read the stable flag, not s->conn — teardown nulls
     * s->conn before releasing, which would misclassify the stream. */
    const bool reactor_mode = s->reactor_owned;

    /* Stream-side cleanup. After this point no H3 callback or
     * coroutine should reach into the slot — only an outstanding PHP
     * HttpRequest wrapper may still hold a request->refcount, in
     * which case the slot stays in the alive list until the wrapper's
     * free_object calls http_request_destroy down to zero. */

    smart_str_free(&s->body_buf);

    if (s->response_body != NULL) {
        zend_string_release(s->response_body);
        s->response_body = NULL;
    }

    if (s->hq_line != NULL) {
        efree(s->hq_line);
        s->hq_line = NULL;
    }

#ifndef PHP_WIN32
    if (s->hq_map != NULL) {
        munmap(s->hq_map, s->hq_map_len);
        s->hq_map = NULL;
    }
#endif

    /* Drain the streaming chunk queue — chunks still owned by us
     * because nghttp3 may have already taken iov pointers but not yet
     * acked them. */
    if (s->chunk_queue != NULL) {
        for (size_t i = s->chunk_queue_head; i < s->chunk_queue_tail; i++) {
            if (s->chunk_queue[i] != NULL) {
                zend_string_release(s->chunk_queue[i]);
            }
        }

        efree(s->chunk_queue);
        s->chunk_queue = NULL;
    }

    /* Static delivery: one-shot reconcile of any in-flight bytes not debited on
     * the ACK path (chunks freed above, or on a submit-failure branch). */
    if (s->tracks_static_bytes && s->static_inflight > 0) {
        h3_static_account_debit(s->static_inflight);
        s->static_inflight = 0;
    }

    /* Reverse-path credit: the stream is going away — unblock a parked
     * producer, then drop the reactor's ref. */
    if (s->wire_credit != NULL) {
        stream_credit_abandon((stream_credit_t *)s->wire_credit);
        s->wire_credit = NULL;
    }

    /* The request can outlive the stream (handler holds its own ref). Sever
     * body_h3_conn so a later pop can't touch a freed connection, and wake
     * a parked consumer if the body never completed. */
    if (s->request != NULL && s->request->body_h3_conn != NULL) {
        s->request->body_h3_conn = NULL;

        if (!s->fin_received) {
            http_body_stream_error(s->request);
        }
    }

    /* Trailer capture (malloc'd in http3_stream_capture_trailers). Held
     * until teardown so the nv stays valid through the async trailer submit. */
    if (s->trailer_nv != NULL) {
        free(s->trailer_nv);
        s->trailer_nv = NULL;
    }

    if (s->trailer_bytes != NULL) {
        free(s->trailer_bytes);
        s->trailer_bytes = NULL;
    }

    if (s->write_event != NULL) {
        zend_async_event_t *ev = &s->write_event->base;

        if (ev->dispose != NULL) {
            ev->dispose(ev);
        }

        s->write_event = NULL;
    }

    /* Unlink from the owning connection's live-stream list. */
    if (s->conn != NULL) {
        if (s->list_prev != NULL) {
            s->list_prev->list_next = s->list_next;
        } else {
            s->conn->streams_head = s->list_next;
        }

        if (s->list_next != NULL) {
            s->list_next->list_prev = s->list_prev;
        }

        s->conn = NULL;
    }

    /* Reactor mode: the per-stream zvals are UNDEF (the reactor never wrapped
     * the request). The slab slot is ours to reclaim — this is the single slab
     * free for the reactor path. The request FIELDS, though, depend on whether
     * the request reached a worker:
     *   - handed off (s->dispatched): the worker freed the fields on its own
     *     thread via http_request_destroy, then posted the consumed that brought
     *     us here. Nothing to free.
     *   - never handed off (early RST / backpressure): no worker ran, so free
     *     the persistent fields here before returning the slot. */
    if (reactor_mode) {
        if (!s->dispatched) {
            http_request_free_fields(s->request);
        }

        http3_stream_pool_free(s->pool, s);
        return;
    }

    /* Per-stream PHP objects. The HttpRequest wrapper's free_object
     * calls http_request_destroy on s->request — that decrement may
     * either reach zero (no other ref) and fire the release callback
     * NOW, or stay positive if the userland code stashed the wrapper
     * somewhere; in the latter case the callback fires later when
     * the wrapper is finally collected. dtor is a no-op when UNDEF. */
    zval_ptr_dtor(&s->request_zv);
    zval_ptr_dtor(&s->response_zv);

    /* Drop our own request refcount. If the wrapper's free_object
     * already ran above, this is the last ref and triggers the
     * release callback. If not, the wrapper holds a pending ref and
     * the callback fires later. */
    http_request_destroy(s->request);
}

