/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+

  HTTP/3 nghttp3 + ngtcp2 callbacks and the dispatch tables that
  reference them.

  Split out of http3_connection.c per audit #8. Owns:
    - all nghttp3 application callbacks (HEADERS/DATA/end-stream/close,
      data_reader, acked_stream_data) plus header-table helpers and
      stream-level rejection;
    - http3_stream_submit_response (header emission);
    - the streaming response vtable (h3_stream_ops + append_chunk /
      mark_ended / get_wait_event);
    - http3_finalize_request_body;
    - all ngtcp2 transport callbacks bridging to nghttp3 (recv_stream_data,
      extend_max_stream_data, acked_stream_data_offset, stream_close /
      stream_reset / stream_stop_sending / extend_max_remote_streams_bidi);
    - the two callback tables (HTTP3_NGHTTP3_CALLBACKS, HTTP3_NGTCP2_CALLBACKS);
    - get_new_connection_id_cb + rand_cb (used by HTTP3_NGTCP2_CALLBACKS);
    - handshake_completed_cb + http3_connection_init_h3 (which constructs
      the nghttp3_conn and binds the three control streams).
*/

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif


#include "http3_internal.h"               /* php.h + Zend/zend_async_API.h +
                                            * ngtcp2 + nghttp3 + openssl/ssl.h +
                                            * http3_connection.h + php_http_server.h */
#include "Zend/zend_hrtime.h"              /* zend_hrtime — request timing */
#include "core/http_protocol_handlers.h"   /* http_protocol_get_handler */
#include "http3_listener.h"                /* http3_listener_server_obj etc. */
#include "http3_packet.h"                  /* http3_packet_compute_sr_token */
#include "http3/http3_stream.h"            /* http3_stream_t */

#include <ngtcp2/ngtcp2_crypto.h>          /* ngtcp2_crypto_* callback ptrs */

#include <string.h>

/* Listener accessors not exposed via http3_listener.h. */
extern http3_packet_stats_t *http3_listener_packet_stats(http3_listener_t *l);
extern const uint8_t *http3_listener_sr_key(const http3_listener_t *l);

/* http3_stream_submit_response forward — used by streaming append_chunk
 * (here) and by http3_dispatch.c on the buffered REST commit path.
 * Declared cross-TU in http3_internal.h. */

/* ------------------------------------------------------------------------
 * ngtcp2 base callbacks: rand + new connection id (with deterministic
 * stateless-reset token)
 * ------------------------------------------------------------------------ */

static void rand_cb(uint8_t *dest, size_t destlen, const ngtcp2_rand_ctx *ctx)
{
    (void)ctx;
    /* ngtcp2 rand_cb has a void return — there is no way to signal a
     * DRBG failure to ngtcp2's internal state machine. Returning
     * silently with a zero-filled buffer would make packet-number
     * encryption masks, padding, and reserved-bit randomization
     * predictable on the wire. The only safe response when the system
     * DRBG fails is to abort the process — supervisor / systemd / pm
     * will restart cleanly and a transient OpenSSL hiccup will not
     * have leaked attacker-observable predictability in the meantime. */
    if (!http3_fill_random(dest, destlen)) {
        zend_error_noreturn(E_ERROR,
            "HTTP/3: OpenSSL RAND_bytes failed (DRBG unavailable). "
            "Cannot continue safely.");
    }
}

static int get_new_connection_id_cb(ngtcp2_conn *conn, ngtcp2_cid *cid,
                                    uint8_t *token, size_t cidlen,
                                    void *user_data)
{
    (void)conn;
    http3_connection_t *c = (http3_connection_t *)user_data;
    /* CID is random per RFC 9000 §5.1 — collision-resistant via DRBG.
     * Stateless-reset token is derived deterministically via
     * HMAC-SHA256(listener_sr_key, cid)[0:16]. The deterministic
     * token is what makes peer-side stateless-reset verification work
     * (peer caches the token from NEW_CONNECTION_ID; when a forged-or-
     * legitimate reset arrives we recompute the same value here). */
    if (!http3_fill_random(cid->data, cidlen)) {
        return NGTCP2_ERR_CALLBACK_FAILURE;
    }
    cid->datalen = cidlen;
    const uint8_t *sr_key = c != NULL ? http3_listener_sr_key(c->listener) : NULL;
    if (sr_key == NULL) {
        return NGTCP2_ERR_CALLBACK_FAILURE;
    }
    /* NGTCP2_STATELESS_RESET_TOKENLEN == 16 — same width our HMAC
     * truncation produces. Static-assert in case the spec ever grows. */
    _Static_assert(NGTCP2_STATELESS_RESET_TOKENLEN == 16,
                   "stateless reset token width must match HMAC truncation");
    http3_packet_compute_sr_token(sr_key, cid->data, cidlen, token);
    return 0;
}

/* ------------------------------------------------------------------------
 * nghttp3 application callbacks — request side (HEADERS, DATA, end-stream)
 * ------------------------------------------------------------------------ */

static void h3_ensure_headers_table(http_request_t *const req)
{
    if (req->headers == NULL) {
        ALLOC_HASHTABLE(req->headers);
        zend_hash_init(req->headers, 16, NULL, ZVAL_PTR_DTOR, 0);
    }
}

static void h3_store_header_value(http_request_t *const req,
                                  const char *name, size_t namelen,
                                  const char *value, size_t valuelen)
{
    h3_ensure_headers_table(req);

    zend_string *const name_str = zend_string_init(name, namelen, 0);
    zend_string *const val_str  = zend_string_init(value, valuelen, 0);

    zval tmp;
    ZVAL_STR(&tmp, val_str);
    zend_hash_update(req->headers, name_str, &tmp);

    zend_string_release(name_str);
}

/* RFC 9114 §4.1.2 — stream-level rejection. Trips when the peer
 * exceeds our HTTP3_MAX_HEADERS_BYTES / HTTP3_MAX_BODY_BYTES caps.
 * Earlier code returned NGHTTP3_ERR_CALLBACK_FAILURE which closes the
 * whole QUIC connection; that's hostile to other multiplexed streams.
 * Instead we mark the stream rejected, ask both nghttp3 and ngtcp2 to
 * shut its read side, and let the caller return 0. Subsequent
 * recv_header/recv_data callbacks short-circuit on s->rejected; the
 * STOP_SENDING + RESET_STREAM frames carry NGHTTP3_H3_REQUEST_REJECTED
 * so the peer learns this specific stream was the problem. */
static void h3_reject_request_stream(http3_connection_t *c,
                                     http3_stream_t *s, int64_t stream_id)
{
    if (s == NULL || s->rejected) return;
    s->rejected = true;
    if (c == NULL) return;
    if (c->nghttp3_conn != NULL) {
        (void)nghttp3_conn_shutdown_stream_read(
            (nghttp3_conn *)c->nghttp3_conn, stream_id);
    }
    if (c->ngtcp2_conn != NULL) {
        /* STOP_SENDING — tell peer we won't read more on this stream. */
        (void)ngtcp2_conn_shutdown_stream_read(
            (ngtcp2_conn *)c->ngtcp2_conn, 0, stream_id,
            NGHTTP3_H3_REQUEST_REJECTED);
        /* RESET_STREAM — abort our write side so the peer's response
         * loop wakes (no half-open stream waiting for HEADERS). */
        (void)ngtcp2_conn_shutdown_stream_write(
            (ngtcp2_conn *)c->ngtcp2_conn, 0, stream_id,
            NGHTTP3_H3_REQUEST_REJECTED);
    }
}

/* nghttp3 → http3_stream bridge.
 *
 * begin_headers fires the first time a request stream surfaces decoded
 * field-section bytes. We allocate the per-stream state and pin it as
 * stream_user_data so subsequent recv_header / recv_data / end_stream
 * find it without a HashTable lookup. The stream is released either via
 * h3_stream_close_cb (peer-side close) or by http3_connection_free's
 * teardown path (connection-level close). Dispatch holds a second
 * reference for the coroutine. */
static int h3_begin_headers_cb(nghttp3_conn *conn, int64_t stream_id,
                               void *conn_user_data, void *stream_user_data)
{
    (void)stream_user_data;
    http3_connection_t *c = (http3_connection_t *)conn_user_data;
    http3_packet_stats_t *stats = c != NULL
        ? http3_listener_packet_stats(c->listener) : NULL;

    http3_stream_t *s = http3_stream_new(stream_id);
    if (s == NULL) {
        return NGHTTP3_ERR_CALLBACK_FAILURE;
    }
    if (nghttp3_conn_set_stream_user_data(conn, stream_id, s) != 0) {
        http3_stream_release(s);
        return NGHTTP3_ERR_CALLBACK_FAILURE;
    }
    /* Mirror the user_data on the ngtcp2 side so callbacks that fire on
     * the QUIC layer (extend_max_stream_data — backpressure)
     * can find the stream without an O(n) lookup. ngtcp2 is the source
     * of truth for transport-level events; nghttp3 for application. */
    if (c != NULL && c->ngtcp2_conn != NULL) {
        (void)ngtcp2_conn_set_stream_user_data(
            (ngtcp2_conn *)c->ngtcp2_conn, stream_id, s);
    }
    /* Track in the connection's live-stream list so connection teardown
     * can force-release streams nghttp3_conn_del would otherwise orphan
     * (it does not fire stream_close on remaining streams). */
    if (c != NULL) {
        s->conn = c;
        s->list_next = c->streams_head;
        c->streams_head = s;
    }
    if (stats != NULL) stats->h3_streams_opened++;
    return 0;
}

static int h3_recv_header_cb(nghttp3_conn *conn, int64_t stream_id,
                             int32_t token, nghttp3_rcbuf *name,
                             nghttp3_rcbuf *value, uint8_t flags,
                             void *conn_user_data, void *stream_user_data)
{
    (void)conn; (void)stream_id; (void)flags;
    http3_connection_t *c = (http3_connection_t *)conn_user_data;
    http3_stream_t *s = (http3_stream_t *)stream_user_data;
    if (s == NULL || s->rejected) {
        return 0;
    }

    nghttp3_vec name_v  = nghttp3_rcbuf_get_buf(name);
    nghttp3_vec value_v = nghttp3_rcbuf_get_buf(value);

    /* Same RFC 7541 §4.1 32-byte overhead accounting H2 uses. */
    const size_t entry_cost = name_v.len + value_v.len + 32;
    if (SIZE_MAX - s->headers_total_bytes < entry_cost
     || s->headers_total_bytes + entry_cost > HTTP3_MAX_HEADERS_BYTES) {
        http3_packet_stats_t *stats = c != NULL
            ? http3_listener_packet_stats(c->listener) : NULL;
        if (stats != NULL) stats->h3_request_oversized++;
        /* RFC 9114: reject this stream, don't kill the connection. */
        h3_reject_request_stream(c, s, stream_id);
        return 0;
    }
    s->headers_total_bytes += entry_cost;

    http_request_t *const req = s->request;
    const char *const n = (const char *)name_v.base;
    const char *const v = (const char *)value_v.base;

    /* Pseudo-headers — token enum lets us skip the strcmp ladder for
     * the four RFC 9114 pseudo names. nghttp3 already validates
     * uniqueness + ordering so we map unconditionally. */
    if (token == NGHTTP3_QPACK_TOKEN__METHOD) {
        if (req->method == NULL) {
            req->method = zend_string_init(v, value_v.len, 0);
        }
        return 0;
    }
    if (token == NGHTTP3_QPACK_TOKEN__PATH) {
        if (req->uri == NULL) {
            req->uri = zend_string_init(v, value_v.len, 0);
        }
        return 0;
    }
    if (token == NGHTTP3_QPACK_TOKEN__AUTHORITY) {
        h3_store_header_value(req, "host", 4, v, value_v.len);
        return 0;
    }
    if (token == NGHTTP3_QPACK_TOKEN__SCHEME) {
        h3_store_header_value(req, "scheme", 6, v, value_v.len);
        return 0;
    }

    h3_store_header_value(req, n, name_v.len, v, value_v.len);

    /* Pre-size the body builder when Content-Length lands so we don't
     * geometric-grow under multi-MiB POSTs. Same trick H2 uses. */
    if (name_v.len == 14 && strncasecmp(n, "content-length", 14) == 0
        && value_v.len < 32) {
        char buf[32];
        memcpy(buf, v, value_v.len);
        buf[value_v.len] = '\0';
        char *end = NULL;
        unsigned long long cl = strtoull(buf, &end, 10);
        if (end != buf && *end == '\0' && cl <= SIZE_MAX) {
            req->content_length = (size_t)cl;
        }
    }
    return 0;
}

static int h3_end_headers_cb(nghttp3_conn *conn, int64_t stream_id,
                             int fin, void *conn_user_data,
                             void *stream_user_data)
{
    (void)conn; (void)stream_id; (void)fin;
    http3_connection_t *c = (http3_connection_t *)conn_user_data;
    http3_stream_t *s     = (http3_stream_t *)stream_user_data;
    if (s == NULL || s->dispatched || s->rejected) {
        return 0;
    }
    /* Dispatch the handler the moment headers are complete,
     * regardless of fin (mirror H2 cb_on_frame_recv on HEADERS+END_HEADERS).
     * Body chunks that arrive after this point feed s->body_buf via
     * h3_recv_data_cb; the handler can call $request->awaitBody() to
     * suspend until h3_end_stream_cb finalizes it and fires body_event. */
    http3_stream_dispatch(c, s);
    return 0;
}

static int h3_recv_data_cb(nghttp3_conn *conn, int64_t stream_id,
                           const uint8_t *data, size_t datalen,
                           void *conn_user_data, void *stream_user_data)
{
    (void)conn; (void)stream_id;
    http3_connection_t *c = (http3_connection_t *)conn_user_data;
    http3_stream_t *s = (http3_stream_t *)stream_user_data;
    if (s == NULL || s->rejected) {
        return 0;
    }

    const size_t current = s->body_buf.s != NULL
        ? ZSTR_LEN(s->body_buf.s) : 0;
    if (SIZE_MAX - current < datalen
     || current + datalen > HTTP3_MAX_BODY_BYTES) {
        http3_packet_stats_t *stats = c != NULL
            ? http3_listener_packet_stats(c->listener) : NULL;
        if (stats != NULL) stats->h3_request_oversized++;
        /* RFC 9114: reject this stream, don't kill the connection. */
        h3_reject_request_stream(c, s, stream_id);
        return 0;
    }

    /* Pre-size on first append if the peer told us Content-Length. */
    if (s->body_buf.s == NULL && s->request->content_length > 0
        && s->request->content_length <= HTTP3_MAX_BODY_BYTES) {
        smart_str_alloc(&s->body_buf, s->request->content_length, 0);
    }
    smart_str_appendl(&s->body_buf, (const char *)data, datalen);
    return 0;
}

/* nghttp3 data_reader. Two modes:
 *   - Buffered (REST/setBody): single-slice payload from response_body
 *     at response_body_offset, EOF on the final slice.
 *   - Streaming (HttpResponse::send loop):
 *     hand one queued chunk per call from queue[chunk_read_idx]. When
 *     the pending region empties: EOF if mark_ended fired, else
 *     NGHTTP3_ERR_WOULDBLOCK so nghttp3 parks the reader until the
 *     next append_chunk calls resume_stream.
 *
 * IMPORTANT: chunks are NOT released here. nghttp3 keeps the iov
 * pointer alive until h3_acked_stream_data_cb fires (the peer ACK'd
 * the bytes). Releasing earlier is a UAF — under packet loss nghttp3
 * retransmits from the same iov, so a freed zend_string would crash. */
static nghttp3_ssize h3_read_data_cb(nghttp3_conn *conn, int64_t stream_id,
                                     nghttp3_vec *vec, size_t veccnt,
                                     uint32_t *pflags,
                                     void *conn_user_data,
                                     void *stream_user_data)
{
    (void)conn; (void)stream_id; (void)conn_user_data;
    http3_stream_t *s = (http3_stream_t *)stream_user_data;
    if (s == NULL || veccnt == 0) {
        *pflags |= NGHTTP3_DATA_FLAG_EOF;
        return 0;
    }

    /* Streaming path — chunk_queue is the source of truth when active. */
    if (s->chunk_queue != NULL) {
        /* Skip past chunks already fully handed to nghttp3. They stay
         * in queue[head..read_idx) holding their refcount until ACK. */
        while (s->chunk_read_idx < s->chunk_queue_tail) {
            zend_string *cur = s->chunk_queue[s->chunk_read_idx];
            if (cur == NULL || s->chunk_read_offset >= ZSTR_LEN(cur)) {
                s->chunk_read_idx++;
                s->chunk_read_offset = 0;
                continue;
            }
            break;
        }

        if (s->chunk_read_idx >= s->chunk_queue_tail) {
            if (s->streaming_ended) {
                *pflags |= NGHTTP3_DATA_FLAG_EOF;
                return 0;
            }
            return NGHTTP3_ERR_WOULDBLOCK;
        }

        zend_string *cur = s->chunk_queue[s->chunk_read_idx];
        const size_t avail = ZSTR_LEN(cur) - s->chunk_read_offset;
        vec[0].base = (uint8_t *)ZSTR_VAL(cur) + s->chunk_read_offset;
        vec[0].len  = avail;
        s->chunk_read_offset    += avail;
        s->chunk_pending_bytes  -= avail;
        return 1;
    }

    /* Buffered REST path — unchanged single-slice semantics. */
    if (s->response_body == NULL) {
        *pflags |= NGHTTP3_DATA_FLAG_EOF;
        return 0;
    }
    const size_t total     = ZSTR_LEN(s->response_body);
    const size_t remaining = (s->response_body_offset < total)
                             ? total - s->response_body_offset : 0;
    if (remaining == 0) {
        *pflags |= NGHTTP3_DATA_FLAG_EOF;
        return 0;
    }
    vec[0].base = (uint8_t *)ZSTR_VAL(s->response_body) + s->response_body_offset;
    vec[0].len  = remaining;
    s->response_body_offset += remaining;
    *pflags |= NGHTTP3_DATA_FLAG_EOF;
    return 1;
}

/* RFC 9114 §4.2 forbids these on HTTP/3. Same blocklist as the H2 path
 * — content-length is implicit from the DATA frames, the rest are
 * connection-specific HTTP/1.1 hop-by-hop fields with no analogue at
 * the framed-protocol layer. */
static bool h3_response_header_allowed(const char *name, size_t len)
{
    if (len == 10 && strncasecmp(name, "connection", 10) == 0)         return false;
    if (len == 10 && strncasecmp(name, "keep-alive", 10) == 0)         return false;
    if (len == 17 && strncasecmp(name, "transfer-encoding", 17) == 0)  return false;
    if (len == 7  && strncasecmp(name, "upgrade", 7)  == 0)            return false;
    if (len == 14 && strncasecmp(name, "content-length", 14) == 0)     return false;
    return true;
}

/* Hard cap on outbound response headers — backstop against a
 * server-side accident that fills HttpResponse with an unbounded
 * header set. 256 covers any sane response and bounds RAM. */
enum { H3_RESPONSE_HEADER_MAX = 256 };

/* Growable nghttp3_nv buffer. Starts on a stack scratch[32]; on first
 * overflow promotes to heap (64), then doubles. Geometric growth keeps
 * realloc cost amortised O(1) per entry. */
typedef struct {
    nghttp3_nv  *nv;        /* current write target — scratch or heap */
    nghttp3_nv  *heap;      /* heap-grown buffer, NULL until first promotion */
    nghttp3_nv  *scratch;   /* immutable pointer to caller's stack scratch */
    size_t       nvi;       /* next write index (also == count emitted) */
    size_t       nvcap;     /* current capacity of *nv */
} h3_nv_buf_t;

/* Append one (name, value) pair. Returns false on hard-cap hit;
 * caller treats false as "stop emitting headers". */
static inline bool h3_nv_push(h3_nv_buf_t *b,
                              zend_string *name, zval *val)
{
    if (UNEXPECTED(b->nvi >= H3_RESPONSE_HEADER_MAX)) {
        return false;
    }
    if (UNEXPECTED(b->nvi == b->nvcap)) {
        const size_t new_cap = (b->heap == NULL) ? 64 : b->nvcap * 2;
        nghttp3_nv *new_buf  = (b->heap == NULL)
            ? emalloc(new_cap * sizeof(*new_buf))
            : erealloc(b->heap, new_cap * sizeof(*new_buf));
        if (b->heap == NULL) {
            memcpy(new_buf, b->scratch, b->nvcap * sizeof(*new_buf));
        }
        b->heap  = new_buf;
        b->nv    = new_buf;
        b->nvcap = new_cap;
    }
    nghttp3_nv *slot = &b->nv[b->nvi++];
    slot->name     = (uint8_t *)ZSTR_VAL(name);
    slot->namelen  = ZSTR_LEN(name);
    slot->value    = (uint8_t *)Z_STRVAL_P(val);
    slot->valuelen = Z_STRLEN_P(val);
    slot->flags    = NGHTTP3_NV_FLAG_NONE;
    return true;
}

/* Build the nghttp3_nv array from the PHP response and submit it on
 * the stream. nghttp3 copies name/value into its QPACK encoder buffer
 * during this call, so the backing zend_string memory only needs to
 * stay alive for the duration of submit_response.
 *
 * Two modes:
 *   - Buffered (REST/setBody): copy response body onto the stream as
 *     `response_body`; data_reader emits one slice + EOF.
 *   - Streaming (HttpResponse::send loop): chunk_queue is the body
 *     source; we skip the body copy entirely. Caller has already
 *     primed the queue with the first chunk by the time we run. */
bool http3_stream_submit_response(http3_connection_t *c,
                                  http3_stream_t *s,
                                  bool streaming)
{
    if (c == NULL || s == NULL || c->nghttp3_conn == NULL
        || Z_ISUNDEF(s->response_zv)) {
        return false;
    }

    zend_object *resp_obj = Z_OBJ(s->response_zv);

    /* :status must come first per RFC 9114 §4.3.1. Stringified into a
     * fixed scratch buffer so its lifetime matches the submit call. */
    char status_buf[8];
    int  status = http_response_get_status(resp_obj);
    if (status <= 0) status = 200;
    int status_len = snprintf(status_buf, sizeof(status_buf), "%d", status);
    if (status_len < 0 || status_len >= (int)sizeof(status_buf)) {
        status_len = 3;
        memcpy(status_buf, "500", 3);
    }

    HashTable *headers = http_response_get_headers(resp_obj);

    /* Single-pass header emit. Scratch covers the common case (≤32
     * nv entries — :status + ~30 headers fits every REST/SSE workload
     * we've seen). h3_nv_push handles overflow promotion and the hard
     * cap; we treat its `false` return as "stop emitting more". */
    nghttp3_nv scratch[32];
    h3_nv_buf_t buf = {
        .nv      = scratch,
        .heap    = NULL,
        .scratch = scratch,
        .nvi     = 0,
        .nvcap   = sizeof(scratch) / sizeof(scratch[0]),
    };

    /* :status is always first and always fits — emit unconditionally. */
    scratch[0].name     = (uint8_t *)":status";
    scratch[0].namelen  = 7;
    scratch[0].value    = (uint8_t *)status_buf;
    scratch[0].valuelen = (size_t)status_len;
    scratch[0].flags    = NGHTTP3_NV_FLAG_NONE;
    buf.nvi = 1;

    if (headers != NULL) {
        zend_string *name;
        zval        *values;
        ZEND_HASH_FOREACH_STR_KEY_VAL(headers, name, values) {
            if (name == NULL) continue;
            if (!h3_response_header_allowed(ZSTR_VAL(name), ZSTR_LEN(name))) continue;
            if (EXPECTED(Z_TYPE_P(values) == IS_STRING)) {
                if (!h3_nv_push(&buf, name, values)) goto headers_done;
            } else if (Z_TYPE_P(values) == IS_ARRAY) {
                zval *v;
                ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(values), v) {
                    if (Z_TYPE_P(v) != IS_STRING) continue;
                    if (!h3_nv_push(&buf, name, v)) goto headers_done;
                } ZEND_HASH_FOREACH_END();
            }
        } ZEND_HASH_FOREACH_END();
    }
headers_done:

    /* Body source. Buffered: copy onto the stream so the data_reader
     * outlives the response zval (released in dispose right after this
     * submit). Streaming: the chunk_queue already has the bytes; the
     * data_reader walks it directly. */
    if (!streaming) {
        zend_string *body_str = http_response_get_body_str(resp_obj);
        if (body_str != NULL && ZSTR_LEN(body_str) > 0) {
            /* Zero-copy: addref the response's underlying zend_string
             * instead of memcpy'ing it. The reader walks it asynchronously,
             * so we must outlive the response object dispose that follows
             * this submit. */
            s->response_body        = zend_string_copy(body_str);
            s->response_body_offset = 0;
        }
    }

    const nghttp3_data_reader dr = { .read_data = h3_read_data_cb };
    int rv = nghttp3_conn_submit_response(
        (nghttp3_conn *)c->nghttp3_conn, s->stream_id,
        buf.nv, buf.nvi, &dr);

    if (buf.heap != NULL) efree(buf.heap);

    http3_packet_stats_t *stats = http3_listener_packet_stats(c->listener);
    if (rv == 0) {
        if (stats != NULL) stats->h3_response_submitted++;
        return true;
    }
    if (stats != NULL) stats->h3_response_submit_error++;
    if (s->response_body != NULL) {
        zend_string_release(s->response_body);
        s->response_body = NULL;
    }
    return false;
}

/* ---------------------------------------------------------------------
 * Streaming response vtable
 *
 * Mirrors h2_stream_ops in shape and discipline. Backpressure path:
 * append_chunk enqueues into chunk_queue, calls
 * nghttp3_conn_resume_stream so the data_reader gets re-invoked, then
 * drives http3_connection_drain_out so packets actually leave the
 * socket. If the per-stream window is exhausted, ngtcp2 stops emitting;
 * the caller suspends on write_event, which extend_max_stream_data_cb
 * fires when the peer extends the window via MAX_STREAM_DATA.
 * ------------------------------------------------------------------- */
static int h3_stream_append_chunk(void *ctx, zend_string *chunk)
{
    http3_stream_t *s = (http3_stream_t *)ctx;
    if (s == NULL || s->conn == NULL || s->peer_closed) {
        zend_string_release(chunk);
        return HTTP_STREAM_APPEND_STREAM_DEAD;
    }
    http3_connection_t *c = s->conn;
    if (c->closed || c->nghttp3_conn == NULL) {
        zend_string_release(chunk);
        return HTTP_STREAM_APPEND_STREAM_DEAD;
    }

    /* First send() — lazy queue + HEADERS commit (submit_response with
     * the streaming data_reader). The data_reader reads the chunk we
     * are about to enqueue. */
    const bool first_call = s->chunk_queue == NULL;
    if (first_call) {
        s->chunk_queue_cap     = 8;
        s->chunk_queue         = ecalloc(s->chunk_queue_cap, sizeof(zend_string *));
        s->chunk_queue_head    = 0;
        s->chunk_read_idx      = 0;
        s->chunk_queue_tail    = 0;
        s->chunk_pending_bytes = 0;
        s->chunk_read_offset   = 0;
        s->chunk_ack_credit    = 0;
    }

    /* Grow ring if full — compact head→0 first to avoid unbounded
     * growth on long-lived streams. Compaction shifts head, read_idx,
     * and tail by the same delta. */
    if (s->chunk_queue_tail == s->chunk_queue_cap) {
        if (s->chunk_queue_head > 0) {
            const size_t shift = s->chunk_queue_head;
            const size_t live  = s->chunk_queue_tail - shift;
            memmove(s->chunk_queue,
                    s->chunk_queue + shift,
                    live * sizeof(zend_string *));
            s->chunk_queue_head -= shift;
            s->chunk_read_idx   -= shift;
            s->chunk_queue_tail -= shift;
        }
        if (s->chunk_queue_tail == s->chunk_queue_cap) {
            const size_t new_cap = s->chunk_queue_cap * 2;
            s->chunk_queue = erealloc(s->chunk_queue,
                                      new_cap * sizeof(zend_string *));
            s->chunk_queue_cap = new_cap;
        }
    }

    s->chunk_queue[s->chunk_queue_tail++] = chunk;
    s->chunk_pending_bytes += ZSTR_LEN(chunk);

    /* Telemetry — match the H1/H2 vantage so operators see one
     * unified streaming-load picture across protocols. */
    http_server_counters_t *counters = c->counters;
    if (first_call) {
        http_server_on_streaming_response_started(counters);
    }
    http_server_on_stream_send(counters, ZSTR_LEN(chunk));

    if (first_call) {
        /* Submit headers + streaming data_reader. body=skip-copy.
         * On submit failure, drain the chunk_queue we just primed —
         * nghttp3 won't ever call our data_reader, so the chunk would
         * leak until connection close. */
        if (!http3_stream_submit_response(c, s, true)) {
            for (size_t i = s->chunk_queue_head; i < s->chunk_queue_tail; i++) {
                if (s->chunk_queue[i] != NULL) {
                    zend_string_release(s->chunk_queue[i]);
                    s->chunk_queue[i] = NULL;
                }
            }
            s->chunk_pending_bytes = 0;
            return HTTP_STREAM_APPEND_STREAM_DEAD;
        }
    } else {
        /* Wake the data_reader — it may be parked on WOULDBLOCK. */
        (void)nghttp3_conn_resume_stream(
            (nghttp3_conn *)c->nghttp3_conn, s->stream_id);
    }

    /* Drive packets out so the queue can drain into the network. */
    http3_connection_drain_out(c);
    http3_connection_arm_timer(c);

    /* Backpressure wait: if bytes are still queued, the stream window
     * (or connection window) is holding them back. Suspend on
     * write_event; extend_max_stream_data_cb fires it when the peer
     * extends the window via MAX_STREAM_DATA. */
    if (s->chunk_pending_bytes > 0) {
        http_server_on_stream_backpressure(counters);
    }
    /* Pull write_timeout_s once — config can't change mid-handler.
     * 0 = wait forever (used in tests / bring-up). Pre-multiply to ms so
     * the inner loop doesn't re-derive it per suspension. */
    const uint32_t write_timeout_ms =
        (uint32_t)c->view->write_timeout_s * 1000u;
    while (s->chunk_pending_bytes > 0 && !s->peer_closed) {
        zend_coroutine_t *co = ZEND_ASYNC_CURRENT_COROUTINE;
        if (co == NULL || ZEND_ASYNC_IS_SCHEDULER_CONTEXT) {
            /* Not in a coroutine — can't suspend; flush is best-effort. */
            break;
        }
        if (s->write_event == NULL) {
            s->write_event = ZEND_ASYNC_NEW_TRIGGER_EVENT();
            if (s->write_event == NULL) {
                return HTTP_STREAM_APPEND_STREAM_DEAD;
            }
        }
        zend_async_event_t *wake_ev =
            &s->write_event->base;
        if (ZEND_ASYNC_WAKER_NEW(co) == NULL) {
            return HTTP_STREAM_APPEND_STREAM_DEAD;
        }
        zend_async_resume_when(co, wake_ev, false,
                               zend_async_waker_callback_resolve, NULL);
        /* Defensive write_timeout_s — caps the wait so a peer that stops
         * acknowledging can't pin the handler forever. Mirrors H2's
         * h2_wait_for_drain_event. Two wake sources, whichever fires
         * first wins. */
        if (write_timeout_ms > 0) {
            zend_async_event_t *timer =
                &ZEND_ASYNC_NEW_TIMER_EVENT(
                    (zend_ulong)write_timeout_ms, false)->base;
            zend_async_resume_when(co, timer, true,
                                   zend_async_waker_callback_timeout, NULL);
        }
        ZEND_ASYNC_SUSPEND();
        zend_async_waker_clean(co);
        if (EG(exception) != NULL) {
            /* Timeout exception expected for genuinely stalled peers;
             * cancel-from-RST also lands here. send() surfaces this as
             * HttpException to the user handler. */
            return HTTP_STREAM_APPEND_STREAM_DEAD;
        }
        /* Window extended — try draining again. */
        http3_connection_drain_out(c);
        http3_connection_arm_timer(c);
    }
    return s->peer_closed ? HTTP_STREAM_APPEND_STREAM_DEAD
                          : HTTP_STREAM_APPEND_OK;
}

static void h3_stream_mark_ended(void *ctx)
{
    http3_stream_t *s = (http3_stream_t *)ctx;
    if (s == NULL || s->conn == NULL || s->streaming_ended) {
        return;
    }
    s->streaming_ended = true;
    if (s->peer_closed || s->conn->closed
        || s->conn->nghttp3_conn == NULL) {
        return;
    }
    /* Wake the data_reader so it sees streaming_ended + empty queue
     * → EOF, and drive a final drain so the EOF actually goes out. */
    (void)nghttp3_conn_resume_stream(
        (nghttp3_conn *)s->conn->nghttp3_conn, s->stream_id);
    http3_connection_drain_out(s->conn);
    http3_connection_arm_timer(s->conn);
}

static zend_async_event_t *h3_stream_get_wait_event(void *ctx)
{
    http3_stream_t *s = (http3_stream_t *)ctx;
    if (s == NULL) return NULL;
    if (s->write_event == NULL) {
        s->write_event = ZEND_ASYNC_NEW_TRIGGER_EVENT();
    }
    return s->write_event != NULL
               ? &s->write_event->base
               : NULL;
}

const http_response_stream_ops_t h3_stream_ops = {
    .append_chunk   = h3_stream_append_chunk,
    .mark_ended     = h3_stream_mark_ended,
    .get_wait_event = h3_stream_get_wait_event,
};

/* ------------------------------------------------------------------------
 * Body finalize + end-stream
 * ------------------------------------------------------------------------ */

/* Finalize the request body and wake any awaitBody() waiter.
 * Mirrors http2_session.c finalize_request_body. Called exclusively from
 * h3_end_stream_cb. The handler coroutine is dispatched earlier in
 * h3_end_headers_cb, so by this point it may already be suspended inside
 * $request->awaitBody() with body_event lazily armed.
 *
 * `s->request` is guaranteed alive here: http3_stream_dispatch addrefs
 * it at handler entry so the stream owns one refcount independently of
 * any PHP-side HttpRequest ref. The handler may have returned and
 * dropped its ref already; refcount is then 1 and the request only
 * frees on http3_stream_release. */
static void http3_finalize_request_body(http3_stream_t *s)
{
    http_request_t *req = s->request;
    if (req == NULL) {
        return;
    }

    /* Move the assembled body bytes into the request. smart_str leaves
     * a NUL-terminated zend_string with refcount 1; we transfer that
     * ownership to req->body and clear our handle. */
    if (s->body_buf.s != NULL) {
        smart_str_0(&s->body_buf);
        req->body = s->body_buf.s;
        s->body_buf.s = NULL;        /* request now owns the storage */
    }
    req->complete = true;
    s->fin_received = true;

    /* Wake handlers blocked on $request->awaitBody(). body_event is
     * created lazily on the first awaitBody call; trigger is a no-op
     * when nobody's listening. Same shape as H1/H2 finalize. */
    if (req->body_event != NULL) {
        zend_async_trigger_event_t *trig =
            (zend_async_trigger_event_t *)req->body_event;
        if (trig->trigger != NULL) {
            trig->trigger(trig);
        }
    }
}

static int h3_end_stream_cb(nghttp3_conn *conn, int64_t stream_id,
                            void *conn_user_data, void *stream_user_data)
{
    (void)conn; (void)stream_id;
    http3_connection_t *c = (http3_connection_t *)conn_user_data;
    http3_stream_t *s = (http3_stream_t *)stream_user_data;
    if (s == NULL || s->rejected) {
        return 0;
    }

    http3_finalize_request_body(s);

    http3_packet_stats_t *stats = c != NULL
        ? http3_listener_packet_stats(c->listener) : NULL;
    if (stats != NULL) stats->h3_request_received++;

    /* Defensive: if h3_end_headers_cb never fired (malformed peer that
     * sent only DATA after a missed HEADERS — nghttp3 should already
     * reject this, but belt-and-braces) dispatch here so the peer never
     * sees a half-open stream with no response. */
    if (!s->dispatched) {
        http3_stream_dispatch(c, s);
    }
    return 0;
}

/* ------------------------------------------------------------------------
 * Stream peer-closed wake + nghttp3 close/reset/stop_sending
 * ------------------------------------------------------------------------ */

/* Mark the stream peer-closed and wake any handler suspended on
 * write_event. After this point append_chunk short-circuits
 * to STREAM_DEAD so HttpResponse::send() unwinds cleanly with an
 * exception; mirrors the H2 peer_closed discipline. */
static void h3_stream_mark_peer_closed(http3_stream_t *s)
{
    if (s == NULL || s->peer_closed) {
        return;
    }
    s->peer_closed = true;
    if (s->write_event != NULL) {
        zend_async_trigger_event_t *trig =
            s->write_event;
        if (trig->trigger != NULL) {
            trig->trigger(trig);
        }
    }
}

static int h3_stream_close_cb(nghttp3_conn *conn, int64_t stream_id,
                              uint64_t app_error_code,
                              void *conn_user_data, void *stream_user_data)
{
    (void)conn; (void)app_error_code;
    http3_stream_t *s = (http3_stream_t *)stream_user_data;
    /* Defensive: clear ngtcp2's stream_user_data so any straggler ngtcp2
     * stream callback (e.g. extend_max_stream_data, ack_stream_data) that
     * fires after nghttp3 has already closed the stream cannot deref the
     * about-to-be-freed http3_stream_t. ngtcp2 and nghttp3 maintain their
     * own per-stream state machines; nothing forces them to fire close in
     * lockstep. */
    http3_connection_t *c = (http3_connection_t *)conn_user_data;
    if (c != NULL && c->ngtcp2_conn != NULL) {
        ngtcp2_conn_set_stream_user_data(
            (ngtcp2_conn *)c->ngtcp2_conn, stream_id, NULL);
    }
    h3_stream_mark_peer_closed(s);
    http3_stream_release(s);
    return 0;
}

static int h3_stop_sending_cb(nghttp3_conn *conn, int64_t stream_id,
                              uint64_t app_error_code,
                              void *conn_user_data, void *stream_user_data)
{
    (void)conn; (void)stream_user_data;
    /* Mirror the request via QUIC STOP_SENDING so the peer knows we
     * gave up on its data. ngtcp2 has the function exposed on the
     * connection-level handle. */
    http3_connection_t *c = (http3_connection_t *)conn_user_data;
    if (c != NULL && c->ngtcp2_conn != NULL) {
        ngtcp2_conn_shutdown_stream_read(
            (ngtcp2_conn *)c->ngtcp2_conn, 0, stream_id, app_error_code);
    }
    return 0;
}

static int h3_reset_stream_cb(nghttp3_conn *conn, int64_t stream_id,
                              uint64_t app_error_code,
                              void *conn_user_data, void *stream_user_data)
{
    (void)conn; (void)stream_user_data;
    http3_connection_t *c = (http3_connection_t *)conn_user_data;
    if (c != NULL && c->ngtcp2_conn != NULL) {
        ngtcp2_conn_shutdown_stream_write(
            (ngtcp2_conn *)c->ngtcp2_conn, 0, stream_id, app_error_code);
    }
    return 0;
}

/* Release a chunk's zend_string only when the peer ACKs the bytes.
 * ack_credit accumulates partial-chunk ACKs; whenever it spans the
 * head chunk's full size we release that chunk and advance head.
 *
 * Releasing earlier (at hand-off in h3_read_data_cb) is a UAF: nghttp3
 * keeps iov pointers alive for retransmit until this callback fires,
 * so freeing on hand-off would corrupt retransmits whenever a packet
 * got lost. */
static int h3_acked_stream_data_cb(nghttp3_conn *conn, int64_t stream_id,
                                   uint64_t datalen, void *cu, void *su)
{
    (void)conn; (void)stream_id; (void)cu;
    http3_stream_t *s = (http3_stream_t *)su;
    if (s == NULL || s->chunk_queue == NULL) {
        return 0;
    }
    s->chunk_ack_credit += datalen;
    while (s->chunk_queue_head < s->chunk_read_idx) {
        zend_string *head = s->chunk_queue[s->chunk_queue_head];
        if (head == NULL) {
            s->chunk_queue_head++;
            continue;
        }
        const size_t hlen = ZSTR_LEN(head);
        if (s->chunk_ack_credit < hlen) {
            break;
        }
        s->chunk_ack_credit -= hlen;
        zend_string_release(head);
        s->chunk_queue[s->chunk_queue_head] = NULL;
        s->chunk_queue_head++;
    }
    return 0;
}

const nghttp3_callbacks HTTP3_NGHTTP3_CALLBACKS = {
    .stream_close       = h3_stream_close_cb,
    .recv_data          = h3_recv_data_cb,
    .stop_sending       = h3_stop_sending_cb,
    .end_stream         = h3_end_stream_cb,
    .reset_stream       = h3_reset_stream_cb,
    .begin_headers      = h3_begin_headers_cb,
    .recv_header        = h3_recv_header_cb,
    .end_headers        = h3_end_headers_cb,
    .acked_stream_data  = h3_acked_stream_data_cb,
    /* Trailers, server-push, datagram, and shutdown callbacks stay NULL. */
};

/* ------------------------------------------------------------------------
 * nghttp3 setup — bind control + QPACK streams once handshake completes
 * ------------------------------------------------------------------------ */

bool http3_connection_init_h3(http3_connection_t *c)
{
    nghttp3_settings settings;
    nghttp3_settings_default(&settings);
    /* QPACK dynamic table stays at 0 capacity (static-only) so we avoid
     * the QPACK encoder-stream dynamic-index fan-out. */

    nghttp3_conn *h3 = NULL;
    int rv = nghttp3_conn_server_new(
        &h3, &HTTP3_NGHTTP3_CALLBACKS, &settings, nghttp3_mem_default(), c);
    if (rv != 0) {
        return false;
    }

    /* Tell nghttp3 the bidi-stream limit we advertised in the QUIC
     * transport params. nghttp3 uses this to refuse work past that
     * cap, matching ngtcp2's own enforcement. Mirror the same
     * resolution http3_connection_make_transport_params used:
     * config setter → bench env override → built-in default. */
    uint64_t bidi_cap = 100;
    {
        const http_server_object *srv_obj =
            (const http_server_object *)http3_listener_server_obj(c->listener);
        const uint32_t cfg_streams = http_server_get_http3_max_concurrent_streams(srv_obj);
        if (cfg_streams != 0) {
            bidi_cap = (uint64_t)cfg_streams;
        }
        if (getenv("PHP_HTTP3_BENCH_FC") != NULL) {
            bidi_cap = 1000000ull;
        }
    }
    nghttp3_conn_set_max_client_streams_bidi(h3, bidi_cap);

    int64_t ctrl_id = -1, qenc_id = -1, qdec_id = -1;
    if (ngtcp2_conn_open_uni_stream((ngtcp2_conn *)c->ngtcp2_conn, &ctrl_id, NULL) != 0
     || ngtcp2_conn_open_uni_stream((ngtcp2_conn *)c->ngtcp2_conn, &qenc_id, NULL) != 0
     || ngtcp2_conn_open_uni_stream((ngtcp2_conn *)c->ngtcp2_conn, &qdec_id, NULL) != 0) {
        nghttp3_conn_del(h3);
        return false;
    }

    if (nghttp3_conn_bind_control_stream(h3, ctrl_id) != 0
     || nghttp3_conn_bind_qpack_streams(h3, qenc_id, qdec_id) != 0) {
        nghttp3_conn_del(h3);
        return false;
    }

    c->nghttp3_conn = h3;
    return true;
}

/* ------------------------------------------------------------------------
 * ngtcp2 transport callbacks (forward into nghttp3 + flow-control wakes)
 * ------------------------------------------------------------------------ */

/* Defense-in-depth ALPN verification. The select callback in tls_layer.c
 * already constrains QUIC to "h3" only — if the client offered no ALPN we
 * sent ALERT_FATAL there, so the handshake should never complete with a
 * non-h3 protocol. But this fires once per handshake and the cost is two
 * compares; it's worth the audit-trail counter and the explicit refusal
 * if some future TLS-stack quirk skips the select callback path. */
static int handshake_completed_cb(ngtcp2_conn *conn, void *user_data)
{
    (void)conn;
    http3_connection_t *c = (http3_connection_t *)user_data;
    if (c == NULL) {
        return NGTCP2_ERR_CALLBACK_FAILURE;
    }

    http3_packet_stats_t *stats = http3_listener_packet_stats(c->listener);

    const unsigned char *proto = NULL;
    unsigned int proto_len = 0;
    SSL_get0_alpn_selected((SSL *)c->ssl, &proto, &proto_len);

    if (proto_len != 2 || proto == NULL || memcmp(proto, "h3", 2) != 0) {
        if (stats != NULL) stats->quic_alpn_mismatch++;
        /* Returning CALLBACK_FAILURE asks ngtcp2 to close the connection;
         * the subsequent drain will emit a CONNECTION_CLOSE frame. */
        return NGTCP2_ERR_CALLBACK_FAILURE;
    }

    if (stats != NULL) stats->quic_handshake_completed++;

    /* Wire nghttp3 only after ALPN passes. A failure here is fatal for
     * this connection — without nghttp3 we cannot speak HTTP/3 to the
     * peer, so close. */
    if (!http3_connection_init_h3(c)) {
        if (stats != NULL) stats->h3_init_failed++;
        return NGTCP2_ERR_CALLBACK_FAILURE;
    }
    if (stats != NULL) stats->h3_init_ok++;
    return 0;
}

static int recv_stream_data_cb(ngtcp2_conn *conn, uint32_t flags,
                               int64_t stream_id, uint64_t offset,
                               const uint8_t *data, size_t datalen,
                               void *user_data, void *stream_user_data)
{
    (void)conn; (void)offset; (void)stream_user_data;
    http3_connection_t *c = (http3_connection_t *)user_data;
    if (c == NULL || c->nghttp3_conn == NULL) {
        return 0;  /* Pre-handshake stream data is not expected; drop. */
    }
    int fin = (flags & NGTCP2_STREAM_DATA_FLAG_FIN) ? 1 : 0;
    nghttp3_ssize n = nghttp3_conn_read_stream(
        (nghttp3_conn *)c->nghttp3_conn, stream_id, data, datalen, fin);
    if (n < 0) {
        http3_packet_stats_t *stats = http3_listener_packet_stats(c->listener);
        if (stats != NULL) stats->h3_stream_read_error++;
        return NGTCP2_ERR_CALLBACK_FAILURE;
    }
    /* Tell ngtcp2 how many bytes nghttp3 actually consumed so it can
     * advance the QUIC flow-control window. */
    ngtcp2_conn_extend_max_stream_offset((ngtcp2_conn *)c->ngtcp2_conn,
                                         stream_id, (uint64_t)n);
    ngtcp2_conn_extend_max_offset((ngtcp2_conn *)c->ngtcp2_conn, (uint64_t)n);
    return 0;
}

/* Peer extended our send window for this stream (or for the
 * connection). Wake the handler if it's parked inside append_chunk so
 * it retries the drain pump. nghttp3 uses a per-stream user_data we
 * set in h3_begin_headers; reuse that pointer here. */
static int extend_max_stream_data_cb(ngtcp2_conn *conn, int64_t stream_id,
                                     uint64_t max_data,
                                     void *user_data, void *stream_user_data)
{
    (void)conn; (void)max_data;
    http3_connection_t *c = (http3_connection_t *)user_data;
    http3_stream_t     *s = (http3_stream_t *)stream_user_data;

    /* Pair with drain_out's nghttp3_conn_block_stream call: we blocked
     * the stream when ngtcp2 returned STREAM_DATA_BLOCKED, but never
     * told nghttp3 the window has reopened. Without this unblock,
     * nghttp3_conn_writev_stream keeps skipping the stream and the
     * data_reader is never re-invoked even though new credits arrived.
     * Symptom: streams that exceed initial cwnd (~17 KiB on cubic)
     * stall mid-response. */
    if (c != NULL && c->nghttp3_conn != NULL) {
        (void)nghttp3_conn_unblock_stream(
            (nghttp3_conn *)c->nghttp3_conn, stream_id);
    }
    if (s != NULL && s->write_event != NULL) {
        zend_async_trigger_event_t *trig =
            s->write_event;
        if (trig->trigger != NULL) {
            trig->trigger(trig);
        }
    }
    return 0;
}

static int acked_stream_data_offset_cb(ngtcp2_conn *conn, int64_t stream_id,
                                       uint64_t offset, uint64_t datalen,
                                       void *user_data, void *stream_user_data)
{
    (void)conn; (void)offset;
    http3_connection_t *c = (http3_connection_t *)user_data;
    http3_stream_t     *s = (http3_stream_t *)stream_user_data;
    if (c == NULL || c->nghttp3_conn == NULL) {
        return 0;
    }
    if (nghttp3_conn_add_ack_offset(
            (nghttp3_conn *)c->nghttp3_conn, stream_id, datalen) != 0) {
        return NGTCP2_ERR_CALLBACK_FAILURE;
    }

    /* Each ACK grows the congestion window. If the handler suspended
     * inside append_chunk because drain_out couldn't emit more (cwnd
     * exhausted, ngtcp2 returns STREAM_DATA_BLOCKED → we called
     * nghttp3_conn_block_stream), wake it now: we have fresh credit.
     * extend_max_stream_data_cb only fires on peer-sent MAX_STREAM_DATA
     * for the per-stream flow window — completely separate from cwnd.
     * Without this wake, large responses stall after the first cwnd
     * burst and never recover. */
    if (s != NULL) {
        (void)nghttp3_conn_unblock_stream(
            (nghttp3_conn *)c->nghttp3_conn, stream_id);
        if (s->write_event != NULL) {
            zend_async_trigger_event_t *trig =
                s->write_event;
            if (trig->trigger != NULL) {
                trig->trigger(trig);
            }
        }
    }
    return 0;
}

static int stream_close_cb(ngtcp2_conn *conn, uint32_t flags,
                           int64_t stream_id, uint64_t app_error_code,
                           void *user_data, void *stream_user_data)
{
    (void)conn; (void)stream_user_data;
    http3_connection_t *c = (http3_connection_t *)user_data;
    if (c == NULL || c->nghttp3_conn == NULL) {
        return 0;
    }
    /* If the stream closed due to a non-app reason (transport-level),
     * nghttp3 wants H3_NO_ERROR rather than the connection error. */
    if (!(flags & NGTCP2_STREAM_CLOSE_FLAG_APP_ERROR_CODE_SET)) {
        app_error_code = NGHTTP3_H3_NO_ERROR;
    }
    int rv = nghttp3_conn_close_stream(
        (nghttp3_conn *)c->nghttp3_conn, stream_id, app_error_code);
    http3_packet_stats_t *stats = http3_listener_packet_stats(c->listener);
    if (stats != NULL) stats->h3_stream_close++;
    if (rv != 0 && rv != NGHTTP3_ERR_STREAM_NOT_FOUND) {
        return NGTCP2_ERR_CALLBACK_FAILURE;
    }
    return 0;
}

static int stream_reset_cb(ngtcp2_conn *conn, int64_t stream_id,
                           uint64_t final_size, uint64_t app_error_code,
                           void *user_data, void *stream_user_data)
{
    (void)conn; (void)final_size; (void)app_error_code; (void)stream_user_data;
    http3_connection_t *c = (http3_connection_t *)user_data;
    if (c == NULL || c->nghttp3_conn == NULL) {
        return 0;
    }
    if (nghttp3_conn_shutdown_stream_read(
            (nghttp3_conn *)c->nghttp3_conn, stream_id) != 0) {
        return NGTCP2_ERR_CALLBACK_FAILURE;
    }
    return 0;
}

static int stream_stop_sending_cb(ngtcp2_conn *conn, int64_t stream_id,
                                  uint64_t app_error_code,
                                  void *user_data, void *stream_user_data)
{
    (void)conn; (void)app_error_code; (void)stream_user_data;
    http3_connection_t *c = (http3_connection_t *)user_data;
    if (c == NULL || c->nghttp3_conn == NULL) {
        return 0;
    }
    if (nghttp3_conn_shutdown_stream_read(
            (nghttp3_conn *)c->nghttp3_conn, stream_id) != 0) {
        return NGTCP2_ERR_CALLBACK_FAILURE;
    }
    return 0;
}

static int extend_max_remote_streams_bidi_cb(ngtcp2_conn *conn,
                                             uint64_t max_streams,
                                             void *user_data)
{
    (void)conn;
    http3_connection_t *c = (http3_connection_t *)user_data;
    if (c == NULL || c->nghttp3_conn == NULL) {
        return 0;
    }
    nghttp3_conn_set_max_client_streams_bidi(
        (nghttp3_conn *)c->nghttp3_conn, max_streams);
    return 0;
}

const ngtcp2_callbacks HTTP3_NGTCP2_CALLBACKS = {
    .recv_client_initial = ngtcp2_crypto_recv_client_initial_cb,
    .recv_crypto_data    = ngtcp2_crypto_recv_crypto_data_cb,
    .encrypt             = ngtcp2_crypto_encrypt_cb,
    .decrypt             = ngtcp2_crypto_decrypt_cb,
    .hp_mask             = ngtcp2_crypto_hp_mask_cb,
    .rand                = rand_cb,
    .get_new_connection_id = get_new_connection_id_cb,
    .update_key          = ngtcp2_crypto_update_key_cb,
    .delete_crypto_aead_ctx   = ngtcp2_crypto_delete_crypto_aead_ctx_cb,
    .delete_crypto_cipher_ctx = ngtcp2_crypto_delete_crypto_cipher_ctx_cb,
    .get_path_challenge_data  = ngtcp2_crypto_get_path_challenge_data_cb,
    .version_negotiation      = ngtcp2_crypto_version_negotiation_cb,
    .handshake_completed      = handshake_completed_cb,
    /* Stream-event bridge into nghttp3. */
    .recv_stream_data         = recv_stream_data_cb,
    .acked_stream_data_offset = acked_stream_data_offset_cb,
    /* Wake stream-write backpressure waiters on MAX_STREAM_DATA. */
    .extend_max_stream_data   = extend_max_stream_data_cb,
    .stream_close             = stream_close_cb,
    .stream_reset             = stream_reset_cb,
    .stream_stop_sending      = stream_stop_sending_cb,
    .extend_max_remote_streams_bidi = extend_max_remote_streams_bidi_cb,
};

