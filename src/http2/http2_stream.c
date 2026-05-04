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

static void http2_stream_release_via_request(http_request_t *req);

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

    /* Wire up the embedded request. _request_storage was zeroed by
     * ecalloc; just set the alias pointer and the release callback so
     * http_request_destroy returns this slot back to ZendMM via efree
     * once the request refcount hits zero. */
    stream->request = &stream->_request_storage;
    stream->request->refcount = 1;
    stream->request->release  = http2_stream_release_via_request;
    return stream;
}

/* Release callback fires from http_request_destroy when the request
 * refcount finally reaches zero. By then http2_stream_release has
 * already done every field-level teardown step; this hook just
 * efrees the slot, which is safe whether the wrapper outlived the
 * stream or not. Mirrors http3_stream_release_via_request. */
static void http2_stream_release_via_request(http_request_t *req)
{
    /* Offset-0 invariant: _request_storage is the first field of
     * http2_stream_t, so they share the same address. */
    http2_stream_t *const stream = (http2_stream_t *)req;
    efree(stream);
}

void http2_stream_release(http2_stream_t *const stream)
{
    if (stream == NULL) {
        return;
    }
    if (--stream->refcount > 0) {
        return;
    }

    /* Stream-side cleanup. The slot itself is NOT efree'd here — the
     * embedded request may still hold an outstanding ref from a PHP
     * HttpRequest wrapper. The release callback installed at alloc
     * time efrees the slot once the wrapper finally releases. */

    smart_str_free(&stream->request_body_buf);

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
     * wrapper's free_object will call http_request_destroy on
     * stream->request; if that hits 0 the release callback efrees
     * the slot now; if not (wrapper outlived stream) the callback
     * fires later on the wrapper's final release. */
    zval_ptr_dtor(&stream->request_zv);
    zval_ptr_dtor(&stream->response_zv);

    /* Drop our own request refcount. Either races to 0 with the
     * wrapper above (slot freed via callback), or stays positive
     * pending the wrapper release. */
    http_request_destroy(stream->request);
}

/* Back-compat name used by the session table destructor. The table
 * holds exactly one reference — removing an entry releases it. */
void http2_stream_free(http2_stream_t *const stream)
{
    http2_stream_release(stream);
}

