#ifndef HTTP3_STREAM_H
#define HTTP3_STREAM_H

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include "php.h"
#include "Zend/zend_async_API.h"   /* zend_coroutine_t, zend_async_trigger_event_t */
#include "zend_smart_str.h"
#include "http1/http_parser.h"     /* http_request_t */

#include <stdbool.h>
#include <stdint.h>

typedef struct _http3_stream_s     http3_stream_t;
typedef struct _http3_connection_s http3_connection_t;  /* defined in http3_connection.h */

/* One per inbound HTTP/3 request stream (bidi, client-initiated).
 * Mirrors http2_stream_t in spirit but carries less state — nghttp3
 * already keeps the framing/header decoder state.
 *
 * **Layout invariant**: `_request_storage` MUST stay the first field.
 * Every API in the codebase that takes an `http_request_t *` is fed
 * `&s->_request_storage`, and an `http_request_t *` recovered from the
 * PHP HttpRequest wrapper or the destroy callback is cast back to
 * `http3_stream_t *` via the same offset-0 trick. Reordering this
 * field breaks both directions of the cast at runtime — no compiler
 * diagnostic. The `request` pointer below is a back-compat alias so
 * existing call sites keep using `s->request->...` without changes. */
struct _http3_stream_s {
    http_request_t    _request_storage;

    /* Alias pointer kept at &_request_storage. Re-set on every pool
     * alloc since the surrounding memset clears it to NULL. Lets the
     * existing s->request->X call sites work unchanged. */
    http_request_t   *request;

    /* QUIC bidi stream id (always client-initiated, low bits 00). */
    int64_t           stream_id;

    /* Body accumulator across nghttp3 recv_data callbacks. On
     * end_stream the completed buffer moves into request->body and
     * this builder is cleared. */
    smart_str         body_buf;

    /* Cumulative decoded-header bytes across HEADERS frames. nghttp3
     * already enforces SETTINGS_MAX_FIELD_SECTION_SIZE; we keep our
     * own counter as a belt-and-braces cap. */
    size_t            headers_total_bytes;

    /* True after end_stream fired and body was finalized. */
    bool              fin_received;

    /* Set when the stream tripped the headers/body cap. RFC 9114 prefers
     * stream-level rejection (H3_REQUEST_REJECTED) over killing the whole
     * connection — once set, recv_header / recv_data short-circuit and
     * end_headers skips dispatch. The matching shutdown_stream_read on
     * both nghttp3 and ngtcp2 is issued at the moment the cap trips. */
    bool              rejected;

    /* True after dispatch handed the request to PHP-land.
     * Until then the stream owns the request and must free it. */
    bool              dispatched;

    /* Buffered response body (REST/setBody path). The data_reader
     * callback feeds nghttp3 from this buffer at offset
     * `response_body_offset` until EOF. Owned by the stream; released
     * on http3_stream_release. Set in http3_stream_submit_response
     * from http_response_get_body; mutually exclusive with the
     * streaming chunk queue below — the data_reader picks one or the
     * other depending on whether HttpResponse::send() was called. */
    zend_string      *response_body;
    size_t            response_body_offset;

    /* Streaming response chunk queue.
     * Active only when the handler called HttpResponse::send(); a plain
     * setBody() handler leaves these NULL and uses response_body above.
     *
     * Three positions instead of H2's two — nghttp3 keeps iov pointers
     * alive for retransmit until acked_stream_data fires, so we cannot
     * release a chunk's zend_string at hand-off time (UAF on retransmit
     * after packet loss). Layout:
     *
     *   [head .. read_idx)  — handed to nghttp3, awaiting peer ACK.
     *                         Released by h3_acked_stream_data_cb as
     *                         ack_credit accumulates past chunk size.
     *   [read_idx .. tail)  — pending; data_reader hands these out next.
     *
     * chunk_pending_bytes counts the pending region only — that's the
     * backpressure signal for append_chunk's suspend loop (we want to
     * suspend until nghttp3 takes the bytes, not until peer ACKs them,
     * else throughput drops to 1 RTT per chunk). */
    zend_string     **chunk_queue;
    size_t            chunk_queue_cap;
    size_t            chunk_queue_head;     /* first not-yet-acked chunk */
    size_t            chunk_read_idx;       /* next chunk for data_reader (>= head) */
    size_t            chunk_queue_tail;     /* next slot to append into */
    size_t            chunk_pending_bytes;  /* bytes not yet handed to nghttp3 */
    size_t            chunk_read_offset;    /* bytes already handed from queue[chunk_read_idx] */
    size_t            chunk_ack_credit;     /* leftover acked bytes not yet applied to head release */

    /* Set by h3_stream_ops.mark_ended (handler called $res->end() on a
     * streaming response). Tells the data_reader to flag EOF when the
     * queue empties instead of NGHTTP3_ERR_WOULDBLOCK. */
    bool              streaming_ended;

    /* Set by h3_stream_close_cb / h3_reset_stream_cb when nghttp3
     * tears down stream state on a peer RST. After this point any
     * nghttp3_conn_resume_stream call for this stream_id is a no-op
     * and any further append_chunk must short-circuit so the handler
     * unwinds cleanly with HttpException(499). */
    bool              peer_closed;

    /* Trigger event the handler awaits on under flow-control
     * backpressure. The data_reader fires it (lazily) when ngtcp2
     * extends the per-stream write window; lazy-created on first wait,
     * disposed in http3_stream_release. */
    zend_async_trigger_event_t *write_event;

    /* Per-stream PHP objects + handler coroutine. HTTP/3 multiplexes N
     * concurrent requests on one QUIC connection, so each stream needs
     * its own (request, response) zval pair — just like H2's
     * http2_stream_t. */
    zval              request_zv;
    zval              response_zv;
    zend_coroutine_t *coroutine;

    /* Back-pointer to the owning connection. Needed by the dispose
     * path so it can submit the response to the correct nghttp3_conn
     * and trigger drain. Non-owning — the connection outlives any
     * stream it carries. */
    http3_connection_t *conn;

    /* Slab pool the slot came from. Stable across the stream's lifetime
     * even when conn is NULLed during teardown — release() needs this
     * to return the slot. */
    struct http3_stream_pool_s *pool;

    /* Lifecycle refcount. Starts at 1 (held by nghttp3 stream_user_data).
     * Once dispatch fires, the handler coroutine bumps to 2. */
    unsigned          refcount;

    /* Intrusive link in the owning connection's live-stream list. The
     * connection walks this list at teardown to force-release any
     * stream nghttp3_conn_del would otherwise orphan (no stream_close
     * callback fires for streams still alive when the conn is torn
     * down — without the walk, each such stream leaks its request +
     * headers + zend_strings). */
    http3_stream_t   *list_next;
};

/* Allocate a stream + its http_request_t from the listener's slab pool.
 * The conn back-pointer is NOT stored here — h3_begin_headers_cb sets
 * s->conn after the stream joins the connection's live-stream list. */
http3_stream_t *http3_stream_new(http3_connection_t *conn, int64_t stream_id);

/* Decrement refcount; release storage when it hits zero. Drops the
 * partial body buffer, frees the request only if dispatch never fired. */
void http3_stream_release(http3_stream_t *s);

#endif /* HTTP3_STREAM_H */
