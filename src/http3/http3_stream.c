#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <php.h>
#include "Zend/zend_async_API.h"   /* zend_async_trigger_event_t dispose */
#include "http3/http3_stream.h"
#include "http3/http3_stream_pool.h"
#include "http3_connection.h"   /* http3_connection_t — list ownership at teardown */
#include "http3_listener.h"     /* http3_listener_stream_pool */


/* Forward decl — defined below; the release cb is a member of
 * http_request_t.release set at allocation time. */
static void http3_stream_release_via_request(http_request_t *req);

http3_stream_t *http3_stream_new(http3_connection_t *conn, int64_t stream_id)
{
    http3_stream_pool_t *pool =
        conn != NULL ? http3_listener_stream_pool(conn->listener) : NULL;

    http3_stream_t *s;
    if (pool != NULL) {
        s = http3_stream_pool_alloc(pool);
        if (UNEXPECTED(s == NULL)) {
            return NULL;
        }
    } else {
        /* Unit-test fallback — no listener, no pool. */
        s = ecalloc(1, sizeof(*s));
    }

    s->stream_id = stream_id;
    s->refcount  = 1;
    s->pool      = pool;   /* NULL on the ecalloc fallback */

    /* Wire up the embedded request. _request_storage was zeroed by the
     * pool memset (or ecalloc); just set the alias pointer and the
     * release callback so http_request_destroy returns this slot to
     * the pool when the request refcount hits zero. */
    s->request = &s->_request_storage;
    s->request->refcount = 1;
    s->request->release  = http3_stream_release_via_request;
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
static void http3_stream_release_via_request(http_request_t *req)
{
    /* Offset-0 invariant: _request_storage is the first field of
     * http3_stream_t, so the same byte address is both. */
    http3_stream_t *const s = (http3_stream_t *)req;

    if (s->pool != NULL) {
        http3_stream_pool_free(s->pool, s);
    } else {
        efree(s);
    }
}

void http3_stream_release(http3_stream_t *const s)
{
    if (s == NULL) {
        return;
    }
    if (--s->refcount > 0) {
        return;
    }

    /* Stream-side cleanup. After this point no H3 callback or
     * coroutine should reach into the slot — only an outstanding PHP
     * HttpRequest wrapper may still hold a request->refcount, in
     * which case the slot stays in the alive list until the wrapper's
     * free_object calls http_request_destroy down to zero. */

    /* Drop any partial body the peer never finished framing. */
    smart_str_free(&s->body_buf);

    /* Release the buffered response body the data_reader was draining
     * from (REST/setBody path). */
    if (s->response_body != NULL) {
        zend_string_release(s->response_body);
        s->response_body = NULL;
    }

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

    /* Trigger event lazily created by streaming-backpressure wait. */
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

