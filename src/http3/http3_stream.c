#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <php.h>
#include "Zend/zend_async_API.h"   /* zend_async_trigger_event_t dispose */
#include "http3/http3_stream.h"
#include "http3_connection.h"   /* http3_connection_t — list ownership at teardown */


http3_stream_t *http3_stream_new(int64_t stream_id)
{
    http3_stream_t *const s = ecalloc(1, sizeof(*s));
    s->stream_id = stream_id;
    s->refcount  = 1;
    /* Empty request — headers HashTable is lazy (allocated when the
     * first non-pseudo header lands), matching the H1/H2 pattern. */
    s->request   = ecalloc(1, sizeof(http_request_t));
    s->request->refcount = 1;
    /* PHP zvals start UNDEF; dispatch fills them right before spawning
     * the handler coroutine. */
    ZVAL_UNDEF(&s->request_zv);
    ZVAL_UNDEF(&s->response_zv);
    return s;
}

void http3_stream_release(http3_stream_t *const s)
{
    if (s == NULL) {
        return;
    }
    if (--s->refcount > 0) {
        return;
    }

    /* Drop any partial body the peer never finished framing. smart_str
     * is NULL-safe. */
    smart_str_free(&s->body_buf);

    /* Release the buffered response body the data_reader was draining
     * from (REST/setBody path). zend_string_release is NULL-safe via
     * the macro. */
    if (s->response_body != NULL) {
        zend_string_release(s->response_body);
        s->response_body = NULL;
    }

    /* Drain the streaming chunk queue. Any chunk still refcount'ed from
     * a send() that didn't flush before teardown (peer reset mid-stream,
     * graceful close with pending bytes) must release its ref so
     * zend_string tracking stays correct. Mirrors http2_stream_release. */
    if (s->chunk_queue != NULL) {
        for (size_t i = s->chunk_queue_head; i < s->chunk_queue_tail; i++) {
            if (s->chunk_queue[i] != NULL) {
                zend_string_release(s->chunk_queue[i]);
            }
        }
        efree(s->chunk_queue);
        s->chunk_queue = NULL;
    }

    /* Trigger event lazily created by the streaming-backpressure wait.
     * Dispose explicitly so TrueAsync's event registry releases backing
     * memory. NULL-safe. */
    if (s->write_event != NULL) {
        zend_async_event_t *ev =
            &s->write_event->base;
        if (ev->dispose != NULL) {
            ev->dispose(ev);
        }
        s->write_event = NULL;
    }

    /* Per-stream PHP objects. dtor is a no-op when UNDEF, which is
     * the only state we ever expect at release time — the dispose
     * path clears them to UNDEF before dropping its ref. */
    zval_ptr_dtor(&s->request_zv);
    zval_ptr_dtor(&s->response_zv);

    /* Refcounted release. Stream owns one ref independent of
     * any PHP HttpRequest ref bumped at dispatch time; destroy is now
     * a refcount decrement and the actual free fires when the last
     * holder releases. Safe whether dispatch fired or not. */
    if (s->request != NULL) {
        http_request_destroy(s->request);
    }
    s->request = NULL;
    /* Unlink from the owning connection's live-stream list. conn is
     * NULL when the connection has already torn down (it walks the
     * list and NULLs the back-pointer first), so this is the
     * graceful-close branch only. */
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
    efree(s);
}

