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


#include "php.h"
#include "Zend/zend_async_API.h"    /* zend_async_trigger_event_t dispose */
#include "http2/http2_stream.h"
#include "http1/http_parser.h"     /* http_request_destroy */

/*
 * Stream lifecycle.
 *
 * One http2_stream_t per concurrent HTTP/2 request on a session.
 * Straight ecalloc; nghttp2 already enforces MAX_CONCURRENT_STREAMS
 * before ever invoking on_begin_headers_cb, so we never race past it.
 */

http2_stream_t *http2_stream_new(http2_session_t *const session,
                                 const uint32_t stream_id)
{
    http2_stream_t *const stream = ecalloc(1, sizeof(*stream));
    stream->session   = session;
    stream->stream_id = stream_id;
    stream->state     = H2_STREAM_OPEN;
    stream->refcount  = 1;        /* held by the session stream table */

    /* Per-stream PHP object slots start UNDEF — dispatch fills them
     * right before spawning the handler coroutine. */
    ZVAL_UNDEF(&stream->request_zv);
    ZVAL_UNDEF(&stream->response_zv);

    /* Allocate an empty http_request_t. Headers HashTable is lazy
     * (allocated on first on_header_cb) to match the HTTP/1 pattern in
     * src/http1/http_parser.c:173. */
    stream->request = ecalloc(1, sizeof(http_request_t));
    stream->request->refcount = 1;
    return stream;
}

void http2_stream_release(http2_stream_t *const stream)
{
    if (stream == NULL) {
        return;
    }
    if (--stream->refcount > 0) {
        return;
    }

    /* Drop any partial request body we accumulated but never moved into
     * request->body (peer reset the stream mid-body, or we rejected it
     * for exceeding HTTP2_MAX_BODY_SIZE). smart_str_free is NULL-safe. */
    smart_str_free(&stream->request_body_buf);

    /* Drain the streaming chunk queue.
     * Any chunk still refcount'ed from a send() that didn't flush
     * before teardown (peer reset mid-stream, graceful close with
     * pending bytes) must release its ref so zend_string tracking
     * stays correct. */
    if (stream->chunk_queue != NULL) {
        for (size_t i = stream->chunk_queue_head; i < stream->chunk_queue_tail; i++) {
            if (stream->chunk_queue[i] != NULL) {
                zend_string_release(stream->chunk_queue[i]);
            }
        }
        efree(stream->chunk_queue);
        stream->chunk_queue = NULL;
    }
    /* write_event — trigger event lazily created by
     * h2_wait_for_drain_event. Dispose explicitly so TrueAsync's
     * event registry releases backing memory. Safe on NULL. */
    if (stream->write_event != NULL) {
        zend_async_event_t *const ev =
            &((zend_async_trigger_event_t *)stream->write_event)->base;
        if (ev->dispose != NULL) {
            ev->dispose(ev);
        }
        stream->write_event = NULL;
    }

    /* Per-stream PHP objects — dtor is a no-op when UNDEF. The
     * handler coroutine's dispose clears them to UNDEF before
     * releasing its ref; the only path that can hit non-UNDEF here
     * is an early teardown before dispatch ever fired. */
    zval_ptr_dtor(&stream->request_zv);
    zval_ptr_dtor(&stream->response_zv);

    /* Refcounted release. The stream owns one ref independent
     * of any PHP HttpRequest ref bumped at dispatch time; release it
     * unconditionally. http_request_destroy decrements; the actual free
     * happens when the last holder releases. */
    if (stream->request != NULL) {
        http_request_destroy(stream->request);
    }
    stream->request = NULL;
    efree(stream);
}

/* Back-compat name used by the session table destructor. The table
 * holds exactly one reference — removing an entry releases it. */
void http2_stream_free(http2_stream_t *const stream)
{
    http2_stream_release(stream);
}

