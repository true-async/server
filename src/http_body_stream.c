/*
 * +----------------------------------------------------------------------+
 * | TrueAsync HTTP Server                                                |
 * +----------------------------------------------------------------------+
 *
 * Streaming request body queue (issue #26). See http_body_stream.h.
 */

#include "http_body_stream.h"

#include "Zend/zend_async_API.h"
#include "Zend/zend_string.h"
#include "Zend/zend_alloc.h"
#include "core/async_plain_event.h"

#ifdef HAVE_HTTP2
#include <nghttp2/nghttp2.h>
#include "http2/http2_session.h"
#endif

#ifdef HAVE_HTTP_SERVER_HTTP3
#include "http3/http3_internal.h"   /* http3_request_body_consume */
#endif

static void fire_data_event(const http_request_t *req)
{
    async_plain_event_fire(req->body_data_event);
}

static bool ensure_data_event(http_request_t *req)
{
    if (req->body_data_event != NULL) {
        return true;
    }

    /* Producer (parser on_data) and consumer (readBody) both run on the
     * same reactor thread — no need for uv_async_t indirection. */
    req->body_data_event = async_plain_event_new();
    return req->body_data_event != NULL;
}

bool http_body_stream_push(http_request_t *req, zend_string *data)
{
    if (UNEXPECTED(!ensure_data_event(req))) {
        req->body_error = true;
        return false;
    }

    http_body_chunk_t *node = emalloc(sizeof(*node));
    node->data = zend_string_copy(data);
    node->next = NULL;

    if (req->body_queue_tail != NULL) {
        req->body_queue_tail->next = node;
    } else {
        req->body_queue_head = node;
    }

    req->body_queue_tail = node;
    req->body_bytes_queued += ZSTR_LEN(data);

    fire_data_event(req);
    return true;
}

void http_body_stream_close(http_request_t *req)
{
    if (req->body_eof) {
        return;
    }

    req->body_eof = true;
    fire_data_event(req);
}

void http_body_stream_error(http_request_t *req)
{
    req->body_error = true;
    http_body_stream_close(req);
}

zend_string *http_body_stream_pop(http_request_t *req)
{
    http_body_chunk_t *node = req->body_queue_head;

    if (node == NULL) {
        return NULL;
    }

    req->body_queue_head = node->next;

    if (req->body_queue_head == NULL) {
        req->body_queue_tail = NULL;
    }

    zend_string *data = node->data;
    efree(node);

    req->body_bytes_queued  -= ZSTR_LEN(data);
    req->body_bytes_consumed += ZSTR_LEN(data);

    /* Flow-control credit: grant peer permission to send another len bytes
     * now that PHP has actually drained them. h_session_schedule_emit pushes
     * the resulting WINDOW_UPDATE on the wire. */
#ifdef HAVE_HTTP2
    if (req->body_h2_session != NULL) {
        http2_session_t *s = req->body_h2_session;
        (void)nghttp2_session_consume(s->ng, req->body_h2_stream_id,
                                      ZSTR_LEN(data));
        h2_session_schedule_emit(s);
    }
#endif

#ifdef HAVE_HTTP_SERVER_HTTP3
    /* Same discipline over QUIC: extend the stream + connection windows for
     * the drained bytes and drive the MAX_STREAM_DATA out. */
    if (req->body_h3_conn != NULL) {
        http3_request_body_consume(req->body_h3_conn,
                                   req->body_h3_stream_id, ZSTR_LEN(data));
    }
#endif

    return data;
}

void http_body_stream_dispose(http_request_t *req)
{
    http_body_chunk_t *node = req->body_queue_head;

    while (node != NULL) {
        http_body_chunk_t *next = node->next;
        zend_string_release(node->data);
        efree(node);
        node = next;
    }

    req->body_queue_head = NULL;
    req->body_queue_tail = NULL;
    req->body_bytes_queued = 0;

    if (req->body_data_event != NULL) {
        if (req->body_data_event->dispose != NULL) {
            req->body_data_event->dispose(req->body_data_event);
        }

        req->body_data_event = NULL;
    }
}
