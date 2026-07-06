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
#include "http3_listener.h"     /* http3_listener_stream_pool */


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
    s->request->persistent =
        (http3_listener_reactor_ctx(conn->listener) != NULL);
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
        /* Bounded mailbox; the reactor drains continuously, so a brief spin on
         * a transient full queue is safe and never deadlocks (the reactor never
         * blocks on the worker). */
        while (!reactor_pool_post_exec(rctx->pool, rctx->reactor_id,
                                       http3_reactor_consumed_apply, s)) {
            /* retry */
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

    /* Capture reactor mode before the unlink below nulls s->conn. In reactor
     * mode the request's lifetime is the worker's (it freed the request fields
     * on its own thread); the slab slot is the reactor's to reclaim here. */
    const bool reactor_mode =
        s->conn != NULL && http3_listener_reactor_ctx(s->conn->listener) != NULL;

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

    /* Reverse-path credit: the stream is going away — unblock a parked
     * producer, then drop the reactor's ref. */
    if (s->wire_credit != NULL) {
        stream_credit_mark_dead((stream_credit_t *)s->wire_credit);
        stream_credit_release((stream_credit_t *)s->wire_credit);
        s->wire_credit = NULL;
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
        http3_stream_t **p = &s->conn->streams_head;
        while (*p != NULL && *p != s) {
            p = &(*p)->list_next;
        }

        if (*p == s) {
            *p = s->list_next;
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

