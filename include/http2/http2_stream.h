/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

#ifndef HTTP2_STREAM_H
#define HTTP2_STREAM_H

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include "php.h"
#include "zend_smart_str.h"
#include "http1/http_parser.h"     /* http_request_t */

#include <stdbool.h>
#include <stdint.h>

typedef struct http2_session_t http2_session_t;
typedef struct http2_stream_t  http2_stream_t;

/* Per-stream state machine — mirrors RFC 9113 §5.1 but collapses
 * (IDLE, RESERVED_LOCAL, RESERVED_REMOTE) into a single IDLE because
 * we never send PUSH_PROMISE (SETTINGS_ENABLE_PUSH = 0). */
typedef enum {
    H2_STREAM_IDLE,
    H2_STREAM_OPEN,
    H2_STREAM_HALF_CLOSED_REMOTE,     /* peer sent END_STREAM */
    H2_STREAM_HALF_CLOSED_LOCAL,      /* we sent END_STREAM */
    H2_STREAM_CLOSED
} http2_stream_state_t;

/* One per concurrent HTTP/2 request on a session. Owns a full
 * http_request_t so the connection layer can hand it to a user
 * handler coroutine the same way it does for HTTP/1.
 *
 * **Layout invariant**: `_request_storage` MUST stay the first field.
 * Every API in the codebase that takes an `http_request_t *` is fed
 * `&s->_request_storage`, and an `http_request_t *` recovered from the
 * PHP HttpRequest wrapper or destroy callback is cast back to
 * `http2_stream_t *` via the same offset-0 trick. Mirrors the
 * http3_stream_t layout. The `request` pointer below is a back-compat
 * alias so existing call sites keep using `s->request->...`. */
struct http2_stream_t {
    http_request_t       _request_storage;

    /* Alias pointer kept at &_request_storage. Re-set on every alloc
     * since memset clears it. Lets existing s->request->X work
     * unchanged. */
    http_request_t      *request;

    http2_session_t     *session;       /* back-ref for callback routing */
    uint32_t             stream_id;
    http2_stream_state_t state;

    /* Cumulative decoded-header bytes across HEADERS + CONTINUATION
     * frames for this stream. Belt-and-braces against
     * CVE-2024-27316 (nghttp2 already caps at
     * SETTINGS_MAX_HEADER_LIST_SIZE; we double-check). */
    size_t               headers_total_bytes;

    /* True after dispatch has fired for this stream — guard against
     * double dispatch in the unlikely case of a malformed CONTINUATION
     * arriving after END_HEADERS (nghttp2 already RST_STREAMs that,
     * but defence-in-depth). */
    bool                 request_dispatched;

    /* Buffered-response state. data_provider reads from response_body
     * starting at response_body_offset. Caller-owned pointer — must
     * stay alive until the DATA END_STREAM frame has drained (tracked
     * externally via want_write). Streaming responses use the chunk
     * queue below instead. */
    const char          *response_body;
    size_t               response_body_len;
    size_t               response_body_offset;

    /* Request body accumulator. Bytes from each
     * on_data_chunk_recv_cb append here; on END_STREAM the completed
     * buffer moves into request->body and this builder is cleared.
     * Kept on the stream (not the request) because it's transient
     * framing state — request->body is the polished result handlers
     * see via $request->getBody(). */
    smart_str            request_body_buf;

    /* Response trailers flag. When true, the
     * DATA data_provider emits NGHTTP2_DATA_FLAG_EOF +
     * NGHTTP2_DATA_FLAG_NO_END_STREAM on the final chunk so nghttp2
     * closes the stream via a terminal HEADERS(trailers) frame
     * instead of END_STREAM on DATA. Set by
     * http2_session_submit_trailer before the drain loop reaches
     * the final DATA slice. */
    bool                 has_trailers;

    /* Streaming-response chunk queue.
     *
     * Active only when the handler called HttpResponse::send(); a
     * plain setBody() handler leaves these NULL and uses the legacy
     * response_body pointer path above.
     *
     * Grow-only ring-ish queue: chunks are appended at tail, drained
     * from head. We never shrink the array — steady-state traffic
     * reaches a plateau. Each slot holds a refcount'ed zend_string
     * handed over from send()'s zval; released once fully drained. */
    zend_string        **chunk_queue;
    size_t               chunk_queue_cap;
    size_t               chunk_queue_head;   /* next chunk to drain from */
    size_t               chunk_queue_tail;   /* next slot to append to */
    size_t               chunk_queue_bytes;  /* undrained bytes across all chunks */
    size_t               chunk_read_offset;  /* bytes already handed to nghttp2 from queue[head] */

    /* True after $res->end() / endWithTrailers() on a streaming
     * response. Data provider uses this to decide EOF vs DEFERRED
     * when the queue empties. */
    bool                 streaming_ended;

    /* Set by cb_on_stream_close when nghttp2 tore down its internal
     * stream state on a peer RST / graceful close. After that point
     * any nghttp2_session_resume_stream_data / submit_* call for this
     * stream_id is unsafe — our strategy-level dispose code checks
     * this before invoking drain on a dead stream. */
    bool                 peer_closed;

    /* Trigger event handler awaits on when backpressure fires. The
     * data provider fires it (on drain) to wake a suspended send().
     * Lazy-created on first need. */
    void                *write_event;  /* zend_async_trigger_event_t *; void to avoid
                                          zend_async_API.h include pollution here */

    /* Per-stream PHP objects. HTTP/2 multiplexes
     * N concurrent requests on one TCP connection — each needs its
     * own HttpRequest / HttpResponse instance. HTTP/1's conn-level
     * request_zv / response_zv fields stay as-is; HTTP/2 uses these
     * instead, populated by the strategy's dispatch path and
     * consumed by http2_handler_coroutine_{entry,dispose}. */
    zval                 request_zv;
    zval                 response_zv;

    /* Handler coroutine for this stream. Set at dispatch time,
     * cleared at the first statement of the dispose handler (same
     * discipline as http_request_t.coroutine — protects against a
     * peer RST_STREAM arriving after dispose begins and double-
     * cancelling). */
    void                *coroutine;    /* zend_coroutine_t *; void to keep the
                                          zend_async header out of this TU */

    /* Lifecycle refcount. nghttp2's on_stream_close_cb
     * can fire while our handler dispose is mid-drain — e.g. the
     * terminal DATA frame triggers stream close inside the same
     * nghttp2_session_mem_send call that commit_stream_response just
     * drove. We can't efree the stream while dispose is still
     * reading from it.
     *
     * Invariant: refcount starts at 1 (held by the session table).
     * Dispatch bumps to 2 (coroutine holds). Each release path
     * decrements; the last release actually efree's. */
    unsigned             refcount;
};

/* Decrement refcount; free storage when it hits zero. */
void http2_stream_release(http2_stream_t *stream);

/* Allocate a stream bound to @p session with the given stream id.
 * Also allocates the stream's http_request_t. Returns NULL on OOM. */
http2_stream_t *http2_stream_new(http2_session_t *session, uint32_t stream_id);

/* Destroy a stream (including its request). Safe with NULL. */
void http2_stream_free(http2_stream_t *stream);

#endif /* HTTP2_STREAM_H */
