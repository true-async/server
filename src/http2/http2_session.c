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
#include "Zend/zend_async_API.h"
#include "Zend/zend_hrtime.h"
#include "php_http_server.h"
#include "core/http_connection.h"
#include "http2/http2_session.h"
#include "http2/http2_stream.h"
#include "http_known_strings.h"
#include "log/trace_context.h"

#include <stdio.h>            /* snprintf for :status value */
#include <string.h>

/*
 * HTTP/2 session wrapper — transport + callbacks + response submission.
 *
 * Built on nghttp2_session (server mode). The only allocations on the
 * hot path are nghttp2's own HPACK / frame buffers; our wrapper adds a
 * stream HashTable (for ownership accounting) and a pending-slice
 * cursor for the mem_send "pointer valid until next call" discipline.
 *
 * Callbacks installed here (see install_callbacks):
 *   - on_begin_headers  — allocate an http2_stream_t + http_request_t,
 *                         attach via nghttp2's stream_user_data so
 *                         subsequent lookups are O(1).
 *   - on_header         — pseudo-header mapping per plan §5.1; regular
 *                         headers into request->headers; per-stream
 *                         header-size cap against CVE-2024-27316.
 *   - on_frame_recv     — dispatch on HEADERS+END_HEADERS (regardless
 *                         of END_STREAM, per plan §3.6 — gRPC bidi
 *                         needs the handler to start before the body).
 *   - on_stream_close   — release the stream back to the table.
 *   - on_data_chunk_recv — accumulate the request body (caps + OOM
 *                         guard).
 */

struct http2_session_t {
    nghttp2_session   *ng;

    /* Owning connection — used by session-level callbacks that need
     * to reach the connection layer directly (without hopping through
     * the strategy trampoline): per-stream handler dispatch under
     * conn->scope, admission control against conn->server (CoDel /
     * REFUSED_STREAM), and telemetry flush on stream close.
     * NULL in unit tests that drive the session without a real
     * connection fixture. */
    http_connection_t *conn;

    /* Dispatch hook — called on HEADERS + END_HEADERS per stream. */
    http2_request_ready_cb_t  on_request_ready;
    void                     *on_request_ready_user_data;

    /* Pending outbound slice — see http2_session_drain. Lives inside
     * nghttp2's internal buffers; we must not free it. Valid until the
     * next nghttp2_session_mem_send call. */
    const uint8_t *send_pending;
    size_t         send_pending_len;
    size_t         send_pending_offset;

    /* Stream table: stream_id → http2_stream_t*. Keeps ownership
     * explicit for teardown; nghttp2 separately holds the same pointer
     * via stream_user_data so callbacks can do O(1) lookups without
     * touching this table. */
    HashTable      streams;

    /* PING RTT state. Our own outbound PINGs embed a
     * zend_hrtime() stamp as the 8-byte opaque payload; when the peer
     * bounces it back with NGHTTP2_FLAG_ACK, on_frame_recv decodes and
     * stores the delta here. nghttp2 auto-ACKs peer-initiated pings
     * without help from us — nothing to measure on that path. */
    uint64_t       last_ping_rtt_ns;

    /* Highest peer-initiated stream-id we have accepted. Peer streams
     * MUST be monotonically increasing per RFC 9113 §5.1.1 — the first
     * use of a new stream-id implicitly closes all lower idle ones, so
     * receiving a smaller id later is a connection error PROTOCOL_ERROR.
     * Set in cb_on_begin_headers; zero means "no peer stream seen yet".
     *
     * Client-initiated streams are also required to be odd; even peer
     * ids on the server side are likewise PROTOCOL_ERROR (reserved for
     * server-initiated push). */
    uint32_t       last_peer_stream_id;

    /* Set when nghttp2_session_mem_recv fails with
     * NGHTTP2_ERR_BAD_CLIENT_MAGIC. At that point the session is
     * unrecoverable and the library will not accept or emit any more
     * frames, so we can't use submit_goaway. The caller checks this
     * flag on the error path and writes a hand-crafted GOAWAY frame
     * directly to the socket before closing. */
    bool           bad_preface_emit_goaway;
};

/* -------------------------------------------------------------------------
 * Stream table helpers
 * ------------------------------------------------------------------------- */

static void stream_table_dtor(zval *pData)
{
    http2_stream_t *const stream = (http2_stream_t *)Z_PTR_P(pData);
    http2_stream_free(stream);
}

static void stream_table_register(http2_session_t *const session,
                                  http2_stream_t *const stream)
{
    zval zv;
    ZVAL_PTR(&zv, stream);
    zend_hash_index_update(&session->streams, stream->stream_id, &zv);
}

static void stream_table_remove(http2_session_t *const session,
                                const uint32_t stream_id)
{
    zend_hash_index_del(&session->streams, stream_id);
}

http2_stream_t *http2_session_find_stream(http2_session_t *const session,
                                          const uint32_t stream_id)
{
    if (session == NULL) {
        return NULL;
    }
    zval *const zv = zend_hash_index_find(&session->streams, stream_id);
    return zv != NULL ? (http2_stream_t *)Z_PTR_P(zv) : NULL;
}

http_connection_t *http2_session_get_conn(http2_session_t *const session)
{
    return session != NULL ? session->conn : NULL;
}

/* -------------------------------------------------------------------------
 * Header storage helpers
 *
 * Matches the HTTP/1 convention (src/http1/http_parser.c) — case-
 * sensitive lowercase keys in request->headers. HTTP/2 requires
 * lowercase names on the wire, so no explicit normalisation is needed.
 * ------------------------------------------------------------------------- */

static void ensure_headers_table(http_request_t *const req)
{
    if (req->headers == NULL) {
        ALLOC_HASHTABLE(req->headers);
        zend_hash_init(req->headers, HTTP_HEADERS_INITIAL_SIZE,
                       NULL, ZVAL_PTR_DTOR, 0);
    }
}

static void store_header_value(http_request_t *const req,
                               const char *const name, const size_t namelen,
                               const char *const value, const size_t valuelen)
{
    ensure_headers_table(req);

    /* Common headers (host, content-length, ...) reuse the process-wide
     * interned zend_string from http_known_strings: zero allocation, no
     * release needed, and HashTable hashes the precomputed key once at
     * MINIT instead of per request. Extension headers fall through to
     * the per-request zend_string_init path. */
    zend_string *name_str = http_known_header_lookup(name, namelen);
    const bool name_owned = (name_str == NULL);
    if (name_owned) {
        name_str = zend_string_init(name, namelen, 0);
    }
    zend_string *const val_str = zend_string_init(value, valuelen, 0);

    zval tmp;
    ZVAL_STR(&tmp, val_str);
    zend_hash_update(req->headers, name_str, &tmp);

    if (name_owned) {
        zend_string_release(name_str);
    }
}

/* -------------------------------------------------------------------------
 * nghttp2 callbacks
 * ------------------------------------------------------------------------- */

/* Frame-header-level gate. nghttp2 invokes this on every inbound
 * frame BEFORE it validates the frame body (HPACK decompression, flow
 * control, stream-state rules). We use this to enforce RFC 9113 §5.1.1
 * peer stream-id rules that nghttp2's own validators treat too softly:
 *
 *  - even-numbered stream-id from the peer → reserved for server push,
 *    a PROTOCOL_ERROR when sent by the client
 *  - stream-id ≤ last accepted peer stream-id → either reuse of a
 *    closed stream (nghttp2 issues a silent RST_STREAM but h2spec
 *    expects a connection-level GOAWAY) or outright monotonicity
 *    violation. Either way we escalate to GOAWAY(PROTOCOL_ERROR).
 *
 * Only HEADERS frames trigger the check — PRIORITY / WINDOW_UPDATE /
 * RST_STREAM on closed streams are allowed by the spec and nghttp2's
 * default handling is correct there. */
static int cb_on_begin_frame(nghttp2_session *const ng,
                             const nghttp2_frame_hd *const hd,
                             void *const user_data)
{
    http2_session_t *const session = (http2_session_t *)user_data;

    if (hd->type != NGHTTP2_HEADERS || hd->stream_id == 0) {
        return 0;
    }
    const uint32_t stream_id = hd->stream_id;

    /* Trailer HEADERS reuse an open stream; nghttp2 tracks that for us,
     * so a stream_id <= last_peer_stream_id is only legal if the stream
     * is still open from our side. Quick O(1) lookup — nghttp2's own
     * stream table via stream_user_data. */
    if (nghttp2_session_get_stream_user_data(ng, stream_id) != NULL) {
        return 0;
    }

    if ((stream_id & 1u) == 0u ||
        stream_id <= session->last_peer_stream_id) {
        (void)nghttp2_submit_goaway(ng, NGHTTP2_FLAG_NONE,
                                    session->last_peer_stream_id,
                                    NGHTTP2_PROTOCOL_ERROR, NULL, 0);
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    }
    return 0;
}

/* Allocate a stream on the first HEADERS frame and attach it as
 * nghttp2's stream_user_data so subsequent on_header / on_data /
 * on_frame / on_stream_close callbacks can retrieve it in O(1). */
static int cb_on_begin_headers(nghttp2_session *const ng,
                               const nghttp2_frame *const frame,
                               void *const user_data)
{
    http2_session_t *const session = (http2_session_t *)user_data;

    /* Only REQUEST category HEADERS create new streams; trailer-
     * category HEADERS reuse an existing stream. */
    if (frame->hd.type != NGHTTP2_HEADERS ||
        frame->headers.cat != NGHTTP2_HCAT_REQUEST) {
        return 0;
    }

    const uint32_t stream_id = frame->hd.stream_id;

    /* RFC 9113 §5.1.1 — peer-initiated streams must carry odd ids and
     * must be monotonically increasing. Receiving an even id from the
     * peer, or an id that is not strictly greater than the highest we
     * have already accepted, is a connection error PROTOCOL_ERROR.
     *
     * nghttp2 itself does not enforce the monotonicity rule against
     * already-closed streams (it only tracks its own next-stream-id for
     * outbound use), so we do it explicitly — otherwise a client could
     * send HEADERS on stream 5, let it close, then reopen stream 3 as
     * a fresh request, which §5.1 forbids. Submit a graceful GOAWAY
     * and short-circuit the callback; nghttp2 flushes the GOAWAY on
     * the next drain and tears the connection down. */
    /* Stream-id monotonicity + odd-id rules enforced earlier in
     * cb_on_begin_frame. By the time we reach here the id is valid; we
     * only need to track the new high-watermark. */
    session->last_peer_stream_id = stream_id;

    /* Admission reject. CoDel or
     * hard-cap already tripped, or in-flight cap reached. Send
     * RST_STREAM(REFUSED_STREAM) — retry-safe per RFC 7540 §8.1.4,
     * gRPC / Envoy clients retry the same request on another replica.
     * Return 0 (not NGHTTP2_ERR_*) so the connection survives and
     * nghttp2 continues processing frames on other streams. */
    if (session->conn != NULL
        && UNEXPECTED(http_server_should_shed_request(session->conn->server))) {
        http_server_on_request_shed(session->conn->counters, /*is_h2=*/true);
        (void)nghttp2_submit_rst_stream(ng, NGHTTP2_FLAG_NONE,
                                        stream_id, NGHTTP2_REFUSED_STREAM);
        return 0;
    }

    http2_stream_t *const stream = http2_stream_new(session, stream_id);
    if (stream == NULL) {
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    }

    stream_table_register(session, stream);
    nghttp2_session_set_stream_user_data(ng, stream_id, stream);

    /* Bump server-level stream telemetry. */
    if (session->conn != NULL) {
        http_server_on_h2_stream_opened(session->conn->counters);
    }
    return 0;
}

/* Populate the stream's request with one name/value pair. Pseudo-
 * headers get mapped per plan §5.1; regular headers go into the
 * request's HashTable unchanged. */
static int cb_on_header(nghttp2_session *const ng,
                        const nghttp2_frame *const frame,
                        const uint8_t *const name, const size_t namelen,
                        const uint8_t *const value, const size_t valuelen,
                        const uint8_t flags, void *const user_data)
{
    (void)flags;
    (void)user_data;

    if (frame->hd.type != NGHTTP2_HEADERS ||
        frame->headers.cat != NGHTTP2_HCAT_REQUEST) {
        return 0;
    }

    http2_stream_t *const stream = (http2_stream_t *)
        nghttp2_session_get_stream_user_data(ng, frame->hd.stream_id);
    if (stream == NULL) {
        return 0;
    }

    /* Belt-and-braces cap against CVE-2024-27316 (plan §4.1). nghttp2
     * enforces SETTINGS_MAX_HEADER_LIST_SIZE first, but we keep our
     * own accumulator so any library regression still has a second
     * line of defence. RFC 7541 §4.1 overhead is 32 bytes per entry. */
    const size_t entry_cost = namelen + valuelen + 32;
    if (SIZE_MAX - stream->headers_total_bytes < entry_cost ||
        stream->headers_total_bytes + entry_cost > HTTP2_SETTINGS_MAX_HEADER_LIST) {
        /* Reset the stream but keep the connection alive for other
         * streams — RFC 9113 §5.4.2 stream-level error handling. */
        return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
    }
    stream->headers_total_bytes += entry_cost;

    http_request_t *const req = stream->request;
    const char *const name_c  = (const char *)name;
    const char *const value_c = (const char *)value;

    /* Pseudo-headers (`:name`) are the four RFC 9113 §8.3.1 fields.
     * nghttp2 validates ordering + absence / duplicates before we see
     * them, so we can map unconditionally. */
    if (namelen >= 1 && name[0] == ':') {
        if (namelen == 7 && memcmp(name, ":method", 7) == 0) {
            if (req->method == NULL) {
                /* Fast-path common methods via the interned pool; fall
                 * back for extension verbs. See src/core/http_known_strings.c. */
                req->method = http_known_method_lookup(value_c, valuelen);
                if (req->method == NULL) {
                    req->method = zend_string_init(value_c, valuelen, 0);
                }
            }
        } else if (namelen == 5 && memcmp(name, ":path", 5) == 0) {
            if (req->uri == NULL) {
                req->uri = zend_string_init(value_c, valuelen, 0);
            }
        } else if (namelen == 10 && memcmp(name, ":authority", 10) == 0) {
            /* Map :authority → Host header per RFC 9113 §8.3.1, so
             * handlers written for HTTP/1 find the origin name where
             * they expect it. */
            store_header_value(req, "host", 4, value_c, valuelen);
        } else if (namelen == 7 && memcmp(name, ":scheme", 7) == 0) {
            /* No natural HTTP/1 mapping; park under `scheme` so PHP
             * handlers can inspect it via $request->getHeader("scheme"). */
            store_header_value(req, "scheme", 6, value_c, valuelen);
        }
        /* Unknown pseudo-headers — nghttp2 already rejects them. */
        return 0;
    }

    store_header_value(req, name_c, namelen, value_c, valuelen);

    /* Populate req->content_length when the Content-Length header lands.
     * Used by cb_on_data_chunk_recv to pre-size the body smart_str in a
     * single allocation instead of taking the geometric-growth path — on
     * multi-MiB POSTs that path explodes to ~40k mremap syscalls per
     * request and serialises under the kernel mmap_sem when several
     * uploads run concurrently. */
    if (namelen == 14 && strncasecmp(name_c, "content-length", 14) == 0) {
        char buf[32];
        if (valuelen < sizeof(buf)) {
            memcpy(buf, value_c, valuelen);
            buf[valuelen] = '\0';
            char *end = NULL;
            const unsigned long long cl = strtoull(buf, &end, 10);
            if (end != buf && *end == '\0' && cl <= SIZE_MAX) {
                req->content_length = (size_t)cl;
            }
        }
    }
    return 0;
}

/* Accumulate a DATA-frame chunk into the stream's request body buffer.
 * Enforces HTTP2_MAX_BODY_SIZE (plan §4) as a belt-and-braces cap on
 * top of the per-stream flow-control window — if the peer ignores
 * the window and somehow keeps shipping bytes, we refuse at this
 * layer too. Returns NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE for a
 * stream-level reset; connection stays up for other streams. */
static int cb_on_data_chunk_recv(nghttp2_session *const ng,
                                 const uint8_t flags,
                                 const int32_t stream_id,
                                 const uint8_t *const data,
                                 const size_t len,
                                 void *const user_data)
{
    (void)flags;
    http2_session_t *const session = (http2_session_t *)user_data;

    http2_stream_t *const stream = (http2_stream_t *)
        nghttp2_session_get_stream_user_data(ng, stream_id);
    if (stream == NULL) {
        return 0;
    }

    /* Request-body byte counter (raw DATA payload, not frame header). */
    if (session != NULL && session->conn != NULL) {
        http_server_on_h2_data_recv(session->conn->counters, len);
    }

    const size_t current = stream->request_body_buf.s != NULL
        ? ZSTR_LEN(stream->request_body_buf.s) : 0;
    /* Shared cap with H1 parser pool; configured via
     * HttpServerConfig::setMaxBodySize(). Fall back to the compile-time
     * default if the global was never initialised (e.g. server running
     * without having been started via http_server_class). */
    size_t body_cap = HTTP_SERVER_G(parser_pool).max_body_size;
    if (body_cap == 0) {
        body_cap = HTTP2_MAX_BODY_SIZE;
    }
    if (SIZE_MAX - current < len ||
        current + len > body_cap) {
        return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
    }

    /* First chunk + known Content-Length: pre-size the smart_str in one
     * allocation. Without this smart_str_appendl doubles the buffer on
     * every overflow, which for a large body turns into tens of
     * thousands of mremap calls — dominant syscall cost under 4-way
     * concurrent uploads because mmap_sem serialises them. With the
     * one-shot reserve the hot path is pure memcpy.
     *
     * Both the pre-size and the append can bailout if memory_limit is
     * tighter than setMaxBodySize. Catch the bailout and return
     * TEMPORAL_CALLBACK_FAILURE — nghttp2 converts that to RST_STREAM
     * (INTERNAL_ERROR) for this stream only; other streams on the
     * same connection survive. Without the guard the longjmp escapes
     * into nghttp2's C frames and the scheduler crashes at
     * shutdown. */
    volatile bool oom = false;
    zend_try {
        if (current == 0 && stream->request != NULL
            && stream->request->content_length > 0
            && stream->request->content_length <= body_cap) {
            (void)smart_str_alloc(&stream->request_body_buf,
                                  stream->request->content_length, 0);
        }

        smart_str_appendl(&stream->request_body_buf, (const char *)data, len);
    } zend_catch {
        oom = true;
    } zend_end_try();
    if (UNEXPECTED(oom)) {
        return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
    }
    return 0;
}

/* Move the accumulated request body buffer into request->body, mark
 * the request complete, and wake any handler currently suspended on
 * awaitBody(). Called on END_STREAM for DATA or HEADERS (the latter
 * for bodiless requests that end right at headers-complete). */
static void finalize_request_body(http2_stream_t *const stream)
{
    http_request_t *const req = stream->request;
    if (req == NULL) {
        return;
    }

    smart_str_0(&stream->request_body_buf);
    if (stream->request_body_buf.s != NULL) {
        /* Transfer ownership: smart_str's allocated buffer becomes
         * request->body. smart_str_extract clears the smart_str so
         * http2_stream_free's smart_str_free is a no-op. */
        req->body = smart_str_extract(&stream->request_body_buf);
    }
    req->complete = true;

    /* Wake handlers blocked on $request->awaitBody(). body_event is
     * created lazily only if something actually awaited — fire path
     * is a no-op when nobody's listening. */
    if (req->body_event != NULL) {
        zend_async_trigger_event_t *const trig =
            (zend_async_trigger_event_t *)req->body_event;
        if (trig->trigger != NULL) {
            trig->trigger(trig);
        }
    }
}

/* on_frame_recv fires for every completed frame. Two interests:
 *   1. HEADERS + END_HEADERS (REQUEST category) → dispatch the handler
 *      coroutine, regardless of END_STREAM (plan §3.6, gRPC bidi).
 *   2. END_STREAM on HEADERS or DATA → finalize the request body
 *      (seal request->body, mark complete, fire body_event waiters).
 * Both can fire on the same HEADERS frame when a client ends the
 * stream without a body (e.g. GET with no body). */
static int cb_on_frame_recv(nghttp2_session *const ng,
                            const nghttp2_frame *const frame,
                            void *const user_data)
{
    http2_session_t *const session = (http2_session_t *)user_data;

    const bool is_request_headers =
        frame->hd.type == NGHTTP2_HEADERS &&
        frame->headers.cat == NGHTTP2_HCAT_REQUEST;

    http2_stream_t *stream = NULL;

    /* Dispatch path. */
    if (is_request_headers &&
        (frame->hd.flags & NGHTTP2_FLAG_END_HEADERS) != 0) {
        stream = (http2_stream_t *)
            nghttp2_session_get_stream_user_data(ng, frame->hd.stream_id);
        if (stream != NULL && !stream->request_dispatched) {
            stream->request_dispatched = true;
            if (session->conn != NULL && session->conn->view != NULL
                && session->conn->view->telemetry_enabled) {
                http_request_parse_trace_context(stream->request);
            }
            /* Bump request refcount BEFORE handing it to PHP.
             * The stream keeps writing body bytes / firing body_event
             * via finalize_request_body even after the user handler may
             * have released its HttpRequest ref; the addref guarantees
             * stream->request stays valid until http2_stream_release. */
            http_request_addref(stream->request);
            if (session->on_request_ready != NULL) {
                session->on_request_ready(stream->request, stream->stream_id,
                                          session->on_request_ready_user_data);
            }
        }
    }

    /* Body-finalize path. END_STREAM can arrive on HEADERS (bodiless
     * request) or on a DATA frame (body ended). */
    if ((frame->hd.flags & NGHTTP2_FLAG_END_STREAM) != 0 &&
        (frame->hd.type == NGHTTP2_DATA || is_request_headers)) {
        if (stream == NULL) {
            stream = (http2_stream_t *)
                nghttp2_session_get_stream_user_data(ng, frame->hd.stream_id);
        }
        if (stream != NULL && stream->request != NULL &&
            !stream->request->complete) {
            finalize_request_body(stream);
        }
    }

    /* PING handling — RTT measurement + gRPC-style graceful shutdown.
     *
     * ACK flag set → our own PING came back, measure RTT from the
     * zend_hrtime() stamp embedded in the opaque payload.
     *
     * ACK flag clear → peer wants us to reply. nghttp2's auto-ACK in
     * nghttp2_session_on_ping_received is guarded by session_is_closing,
     * which returns true once want_read == want_write == 0 — the exact
     * state we end up in after the peer sends a GOAWAY to us on an
     * otherwise idle connection. Without help, the ACK never fires and
     * h2spec (and real gRPC shutdown sequences — see nghttp2 #1103) hang
     * until their own timeout. Submit the ACK ourselves: nghttp2's
     * session_prep_frame for PING only blocks on the TERM_ON_SEND flag
     * (PR #1104, 2018), which we do not set on peer-initiated GOAWAY. */
    /* Peer-sent GOAWAY visibility — real gRPC/Envoy deployments send
     * GOAWAY during rolling restart; operators want the counter to
     * correlate restarts with backend churn. */
    if (frame->hd.type == NGHTTP2_GOAWAY
        && session->conn != NULL) {
        http_server_on_h2_goaway_recv(session->conn->counters);
    }

    if (frame->hd.type == NGHTTP2_PING) {
        if ((frame->hd.flags & NGHTTP2_FLAG_ACK) != 0) {
            uint64_t sent_ns = 0;
            memcpy(&sent_ns, frame->ping.opaque_data, 8);
            const uint64_t now_ns = (uint64_t)zend_hrtime();
            if (now_ns > sent_ns) {
                const uint64_t rtt = now_ns - sent_ns;
                session->last_ping_rtt_ns = rtt;
                if (session->conn != NULL) {
                    http_server_on_h2_ping_rtt(session->conn->counters, rtt);
                }
            }
        } else {
            (void)nghttp2_submit_ping(ng, NGHTTP2_FLAG_ACK,
                                      frame->ping.opaque_data);
        }
    }

    /* WINDOW_UPDATE → wake handler coroutines that suspended on
     * HTTP/2 flow-control backpressure.
     *
     * We arrive here from the read-callback path (scheduler context,
     * handler is suspended). Firing the stream's write_event hands
     * control back to the suspended handler, which then retries its
     * drain. Race-free by construction: fire-from-scheduler always
     * happens AFTER register-from-handler, since the handler had to
     * yield for the read path to run at all.
     *
     * Scope:
     *   - stream_id != 0 → stream-scoped window opened, wake only
     *     that stream's handler.
     *   - stream_id == 0 → connection-scoped window opened; wake
     *     every active stream since any of them may have been
     *     blocked by the connection cap (cheap: N is ≤
     *     MAX_CONCURRENT_STREAMS = 100). */
    if (frame->hd.type == NGHTTP2_WINDOW_UPDATE) {
        if (frame->hd.stream_id != 0) {
            http2_stream_t *const s = (http2_stream_t *)
                nghttp2_session_get_stream_user_data(ng, frame->hd.stream_id);
            if (s != NULL && s->write_event != NULL) {
                zend_async_trigger_event_t *const trig =
                    (zend_async_trigger_event_t *)s->write_event;
                if (trig->trigger != NULL) { trig->trigger(trig); }
            }
        } else {
            zval *zv;
            ZEND_HASH_FOREACH_VAL(&session->streams, zv) {
                http2_stream_t *const s = (http2_stream_t *)Z_PTR_P(zv);
                if (s != NULL && s->write_event != NULL) {
                    zend_async_trigger_event_t *const trig =
                        (zend_async_trigger_event_t *)s->write_event;
                    if (trig->trigger != NULL) { trig->trigger(trig); }
                }
            } ZEND_HASH_FOREACH_END();
        }
    }

    return 0;
}

/* Stream closed on either side — return storage to the pool. nghttp2
 * invokes this exactly once per stream, regardless of which side
 * initiated the close. */
static int cb_on_stream_close(nghttp2_session *const ng,
                              const int32_t stream_id,
                              const uint32_t error_code,
                              void *const user_data)
{
    http2_session_t *const session = (http2_session_t *)user_data;
    if (stream_id <= 0) {
        /* Stream 0 is the connection itself — not a stream. */
        return 0;
    }

    /* Peer-initiated reset: cancel the handler coroutine
     * explicitly with HttpException so user-level `finally` blocks run
     * and backpressure samples don't skew. Before this, we relied on
     * the stream dying under the handler's feet; now the exception is
     * the signalling channel, matching the HTTP/1 parse-error cancel
     * path in src/core/http_connection.c.
     *
     * Only fire when (a) the stream still has a live handler coroutine
     * (dispose hasn't started yet — dispose clears stream->coroutine
     * up front precisely to shut this window) and (b) the close is
     * abnormal (error_code != NO_ERROR). NO_ERROR means a clean
     * END_STREAM on both sides and needs no intervention.
     *
     * session->conn == NULL in offline unit tests, so guard the PHP
     * object-creation path behind it — http_exception_ce isn't
     * registered in the test harness runtime until php_test_runtime_init. */
    http2_stream_t *const stream = (http2_stream_t *)
        nghttp2_session_get_stream_user_data(ng, stream_id);

    /* Peer-initiated reset visibility (RST_STREAM with a non-NO_ERROR
     * code). NO_ERROR is a clean END_STREAM on both sides, not a reset. */
    if (error_code != NGHTTP2_NO_ERROR
        && session->conn != NULL) {
        http_server_on_h2_stream_reset_by_peer(session->conn->counters);
    }

    /* nghttp2's internal stream state is being torn down. Mark the
     * strategy-side stream object so dispose-path drain attempts
     * (h2_stream_mark_ended) know not to poke nghttp2 with a stream_id
     * that no longer has a data provider — doing so segfaults inside
     * nghttp2_session_mem_send when it tries to invoke the callback
     * on already-cleared internal state. */
    if (stream != NULL) {
        stream->peer_closed = true;
    }

    if (stream != NULL && stream->coroutine != NULL &&
        error_code != NGHTTP2_NO_ERROR && http_exception_ce != NULL) {
        zend_coroutine_t *const co = (zend_coroutine_t *)stream->coroutine;

        /* Break back-pointers FIRST — dispose's own invariant, kept in
         * sync so a re-entry (e.g. dispose's commit path triggers
         * another on_stream_close) can't double-cancel. */
        stream->coroutine = NULL;
        if (stream->request != NULL) {
            stream->request->coroutine = NULL;
        }

        zval exc_zv, message_zv, code_zv;
        object_init_ex(&exc_zv, http_exception_ce);
        zend_object *const exc = Z_OBJ(exc_zv);

        ZVAL_STRING(&message_zv, "stream reset by peer");
        zend_update_property_ex(http_exception_ce, exc,
                                ZSTR_KNOWN(ZEND_STR_MESSAGE), &message_zv);
        zval_ptr_dtor(&message_zv);

        /* 499 — nginx convention for "client closed request". Gives
         * user handlers a distinguishable status if they catch the
         * HttpException and want to branch on peer-reset specifically. */
        ZVAL_LONG(&code_zv, 499);
        zend_update_property_ex(http_exception_ce, exc,
                                ZSTR_KNOWN(ZEND_STR_CODE), &code_zv);

        ZEND_ASYNC_CANCEL(co, exc, true);
    }

    stream_table_remove(session, (uint32_t)stream_id);
    if (session->conn != NULL) {
        http_server_on_h2_stream_closed(session->conn->counters);
    }
    return 0;
}

static void install_callbacks(nghttp2_session_callbacks *const cbs)
{
    nghttp2_session_callbacks_set_on_begin_frame_callback(cbs, cb_on_begin_frame);
    nghttp2_session_callbacks_set_on_begin_headers_callback(cbs, cb_on_begin_headers);
    nghttp2_session_callbacks_set_on_header_callback       (cbs, cb_on_header);
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(cbs, cb_on_data_chunk_recv);
    nghttp2_session_callbacks_set_on_frame_recv_callback   (cbs, cb_on_frame_recv);
    nghttp2_session_callbacks_set_on_stream_close_callback (cbs, cb_on_stream_close);
}

/* -------------------------------------------------------------------------
 * Internal helpers
 * ------------------------------------------------------------------------- */

static void apply_hardened_options(nghttp2_option *const opt)
{
    nghttp2_option_set_max_settings(opt, HTTP2_OPT_MAX_SETTINGS);
    nghttp2_option_set_max_outbound_ack(opt, HTTP2_OPT_MAX_OUTBOUND_ACK);
}

static int submit_initial_settings(http2_session_t *const session)
{
    static const nghttp2_settings_entry iv[] = {
        { NGHTTP2_SETTINGS_ENABLE_PUSH,            0                            },
        { NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, HTTP2_SETTINGS_MAX_CONCURRENT },
        { NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE,    HTTP2_SETTINGS_INITIAL_WINDOW },
        { NGHTTP2_SETTINGS_MAX_HEADER_LIST_SIZE,   HTTP2_SETTINGS_MAX_HEADER_LIST },
        { NGHTTP2_SETTINGS_HEADER_TABLE_SIZE,      HTTP2_SETTINGS_HEADER_TABLE_BYTES },
        { NGHTTP2_SETTINGS_MAX_FRAME_SIZE,         HTTP2_SETTINGS_MAX_FRAME },
    };

    if (nghttp2_submit_settings(session->ng, NGHTTP2_FLAG_NONE, iv,
                                sizeof(iv) / sizeof(iv[0])) != 0) {
        return -1;
    }

    (void)nghttp2_session_set_local_window_size(
        session->ng, NGHTTP2_FLAG_NONE, 0,
        (int32_t)HTTP2_SETTINGS_INITIAL_WINDOW);

    return 0;
}

/* -------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

http2_session_t *http2_session_new(http_connection_t *const conn,
                                   const http2_request_ready_cb_t on_request_ready,
                                   void *const user_data)
{
    http2_session_t *session = ecalloc(1, sizeof(*session));
    session->conn                      = conn;
    session->on_request_ready          = on_request_ready;
    session->on_request_ready_user_data = user_data;

    zend_hash_init(&session->streams, 16,
                   NULL, stream_table_dtor, 0);

    nghttp2_option *opt = NULL;
    if (nghttp2_option_new(&opt) != 0) {
        zend_hash_destroy(&session->streams);
        efree(session);
        return NULL;
    }
    apply_hardened_options(opt);

    nghttp2_session_callbacks *cbs = NULL;
    if (nghttp2_session_callbacks_new(&cbs) != 0) {
        nghttp2_option_del(opt);
        zend_hash_destroy(&session->streams);
        efree(session);
        return NULL;
    }
    install_callbacks(cbs);

    const int rc = nghttp2_session_server_new2(
        &session->ng, cbs, session, opt);

    nghttp2_session_callbacks_del(cbs);
    nghttp2_option_del(opt);

    if (rc != 0) {
        zend_hash_destroy(&session->streams);
        efree(session);
        return NULL;
    }

    if (submit_initial_settings(session) != 0) {
        nghttp2_session_del(session->ng);
        zend_hash_destroy(&session->streams);
        efree(session);
        return NULL;
    }

    return session;
}

void http2_session_free(http2_session_t *session)
{
    if (session == NULL) {
        return;
    }
    /* nghttp2_session_del invokes on_stream_close for every live stream;
     * our stream_table_dtor then frees each http2_stream_t. Tear down
     * in that order — the other way round would make the callbacks
     * see a destroyed table. */
    if (session->ng != NULL) {
        nghttp2_session_del(session->ng);
        session->ng = NULL;
    }
    zend_hash_destroy(&session->streams);
    efree(session);
}

int http2_session_feed(http2_session_t *const session,
                       const char *const data, const size_t len,
                       size_t *const consumed_out)
{
    if (session == NULL || session->ng == NULL) {
        if (consumed_out != NULL) { *consumed_out = 0; }
        return -1;
    }
    const ssize_t n = (ssize_t)nghttp2_session_mem_recv(
        session->ng, (const uint8_t *)data, len);

    if (n < 0) {
        /* RFC 9113 §3.4 — on an invalid connection preface the server
         * SHOULD emit a connection-level GOAWAY(PROTOCOL_ERROR) before
         * closing. nghttp2's default behaviour returns
         * NGHTTP2_ERR_BAD_CLIENT_MAGIC (−505) with no outbound frame
         * queued, which looks like an unexplained EOF to the peer.
         * Terminate the session cleanly here — terminate_session
         * queues a GOAWAY, and the caller's next drain() flushes it
         * before the connection layer closes the socket. */
        if (n == NGHTTP2_ERR_BAD_CLIENT_MAGIC) {
            /* Session is already unrecoverable at this point —
             * submit_goaway / terminate_session no longer queue anything
             * because the session state is NGHTTP2_GOAWAY_NONE wasn't
             * even reached. Fall back to a hand-crafted GOAWAY byte
             * stream; the caller writes these raw to the wire. */
            session->bad_preface_emit_goaway = true;
        }
        if (consumed_out != NULL) { *consumed_out = 0; }
        return -1;
    }

    if (consumed_out != NULL) { *consumed_out = (size_t)n; }
    return 0;
}

ssize_t http2_session_drain(http2_session_t *const session,
                            char *const out_buf, const size_t cap)
{
    if (session == NULL || session->ng == NULL) {
        return -1;
    }
    if (cap == 0) {
        return 0;
    }

    size_t written = 0;

    while (written < cap) {
        if (session->send_pending_offset < session->send_pending_len) {
            const size_t avail =
                session->send_pending_len - session->send_pending_offset;
            const size_t space = cap - written;
            const size_t copy  = avail < space ? avail : space;

            memcpy(out_buf + written,
                   session->send_pending + session->send_pending_offset,
                   copy);
            session->send_pending_offset += copy;
            written += copy;

            if (session->send_pending_offset < session->send_pending_len) {
                break;
            }
        }

        const uint8_t *slice = NULL;
        const ssize_t n = (ssize_t)nghttp2_session_mem_send(
            session->ng, &slice);

        if (n < 0) {
            return -1;
        }
        if (n == 0) {
            session->send_pending = NULL;
            session->send_pending_len = 0;
            session->send_pending_offset = 0;
            break;
        }

        session->send_pending = slice;
        session->send_pending_len = (size_t)n;
        session->send_pending_offset = 0;
    }

    return (ssize_t)written;
}

/* Static raw GOAWAY frame for the bad-preface path. See header. */
static const uint8_t BAD_PREFACE_GOAWAY_BYTES[HTTP2_BAD_PREFACE_GOAWAY_LEN] = {
    0x00, 0x00, 0x08,           /* length = 8 (4 last_stream + 4 errcode) */
    0x07,                       /* type  = GOAWAY */
    0x00,                       /* flags = 0 */
    0x00, 0x00, 0x00, 0x00,     /* stream_id = 0 (connection) */
    0x00, 0x00, 0x00, 0x00,     /* last_stream_id = 0 */
    0x00, 0x00, 0x00, 0x01,     /* error_code = PROTOCOL_ERROR */
};

const uint8_t *http2_session_bad_preface_goaway_bytes(void)
{
    return BAD_PREFACE_GOAWAY_BYTES;
}

bool http2_session_should_emit_bad_preface_goaway(
                                const http2_session_t *const session)
{
    return session != NULL && session->bad_preface_emit_goaway;
}

bool http2_session_want_read(const http2_session_t *const session)
{
    return session != NULL && session->ng != NULL &&
           nghttp2_session_want_read(session->ng) != 0;
}

bool http2_session_want_write(const http2_session_t *const session)
{
    if (session == NULL || session->ng == NULL) {
        return false;
    }
    if (session->send_pending_offset < session->send_pending_len) {
        return true;
    }
    return nghttp2_session_want_write(session->ng) != 0;
}

/* -------------------------------------------------------------------------
 * Response submission
 * ------------------------------------------------------------------------- */

/* Data provider — nghttp2 invokes this when it needs DATA-frame bytes.
 * Reads from stream->response_body + offset. Sets
 * NGHTTP2_DATA_FLAG_EOF on the final slice so nghttp2 emits
 * END_STREAM. Zero-copy: we never copy body bytes through an
 * intermediate buffer, the caller's pointer survives until want_write
 * drains. */
/* Stream bodies come from two sources depending on handler mode:
 *  - Buffered (`$res->setBody($str); $res->end();`): a single zero-
 *    copy pointer + length. chunk_queue is NULL.
 *  - Streaming (`$res->send($chunk); ... $res->end();`): a ring-shaped
 *    queue of refcounted zend_strings, drained head-first.
 *    streaming_ended flags the final EOF.
 *
 * This callback handles both uniformly: if chunk_queue is non-NULL
 * we walk the queue; else fall back to the legacy response_body path.
 * When the streaming queue is transiently empty (handler between
 * send() calls), return NGHTTP2_ERR_DEFERRED — nghttp2 parks the
 * data provider until nghttp2_session_resume_data is called from
 * the next send() / end(). */
static ssize_t http2_response_data_read(nghttp2_session *const ng,
                                        const int32_t stream_id,
                                        uint8_t *const buf,
                                        const size_t length,
                                        uint32_t *const data_flags,
                                        nghttp2_data_source *const source,
                                        void *const user_data)
{
    (void)ng;
    (void)stream_id;
    http2_session_t *const ds_session = (http2_session_t *)user_data;

    http2_stream_t *const stream = (http2_stream_t *)source->ptr;

    /* Streaming path — walk the chunk queue. */
    if (stream->chunk_queue != NULL) {
        size_t written = 0;

        while (written < length
               && stream->chunk_queue_head < stream->chunk_queue_tail) {
            zend_string *const chunk =
                stream->chunk_queue[stream->chunk_queue_head];
            const size_t chunk_len  = ZSTR_LEN(chunk);
            const size_t avail      = chunk_len - stream->chunk_read_offset;
            const size_t space      = length - written;
            const size_t to_copy    = avail < space ? avail : space;

            if (to_copy > 0) {
                memcpy(buf + written,
                       ZSTR_VAL(chunk) + stream->chunk_read_offset,
                       to_copy);
                stream->chunk_read_offset += to_copy;
                stream->chunk_queue_bytes -= to_copy;
                written += to_copy;
            }

            /* Chunk fully drained — release refcount, advance head. */
            if (stream->chunk_read_offset == chunk_len) {
                zend_string_release(chunk);
                stream->chunk_queue[stream->chunk_queue_head] = NULL;
                stream->chunk_queue_head++;
                stream->chunk_read_offset = 0;
            }
        }

        /* Queue empty — decide EOF vs DEFERRED. */
        if (stream->chunk_queue_head == stream->chunk_queue_tail) {
            if (stream->streaming_ended) {
                *data_flags |= NGHTTP2_DATA_FLAG_EOF;
                if (stream->has_trailers) {
                    *data_flags |= NGHTTP2_DATA_FLAG_NO_END_STREAM;
                }
            } else if (written == 0) {
                /* Park. The handler is still running and has more to
                 * send — nghttp2 will retry once we call
                 * nghttp2_session_resume_data(stream_id). */
                return NGHTTP2_ERR_DEFERRED;
            }
            /* If written > 0 and !streaming_ended: we return the
             * partial fill; nghttp2 will ask again for more, at
             * which point the queue being still empty falls into
             * the DEFERRED branch above. */
        }

        if (written > 0 && ds_session != NULL && ds_session->conn != NULL) {
            http_server_on_h2_data_sent(ds_session->conn->counters,
                                        (size_t)written);
        }
        return (ssize_t)written;
    }

    /* Buffered path (legacy) — single-pointer zero-copy read. */
    const size_t left =
        stream->response_body_len - stream->response_body_offset;
    const size_t to_copy = left < length ? left : length;

    if (to_copy > 0) {
        memcpy(buf, stream->response_body + stream->response_body_offset,
               to_copy);
        stream->response_body_offset += to_copy;
        if (ds_session != NULL && ds_session->conn != NULL) {
            http_server_on_h2_data_sent(ds_session->conn->counters, to_copy);
        }
    }

    if (stream->response_body_offset >= stream->response_body_len) {
        *data_flags |= NGHTTP2_DATA_FLAG_EOF;
        if (stream->has_trailers) {
            /* Defer END_STREAM — nghttp2 will emit a terminal
             * HEADERS(trailers) frame carrying END_STREAM instead
             * (queued via nghttp2_submit_trailer). */
            *data_flags |= NGHTTP2_DATA_FLAG_NO_END_STREAM;
        }
    }
    return (ssize_t)to_copy;
}

int http2_session_submit_response(http2_session_t *const session,
                                  const uint32_t stream_id,
                                  const int status,
                                  const http2_header_view_t *const headers,
                                  const size_t headers_len,
                                  const char *const body,
                                  const size_t body_len)
{
    if (session == NULL || session->ng == NULL) {
        return -1;
    }
    if (status < 100 || status > 999) {
        return -1;
    }

    http2_stream_t *const stream = http2_session_find_stream(session,
                                                             stream_id);
    if (stream == NULL) {
        return -1;
    }

    /* Pin the body on the stream so the data_provider can find it
     * without a side table. Caller retains ownership. */
    stream->response_body        = body;
    stream->response_body_len    = body_len;
    stream->response_body_offset = 0;

    /* Build nghttp2_nv[]. Scratch lives on the stack for the common
     * case; heap fallback only when total exceeds HTTP2_NV_SCRATCH. */
    nghttp2_nv nv_scratch[HTTP2_NV_SCRATCH];
    nghttp2_nv *nv      = nv_scratch;
    nghttp2_nv *nv_heap = NULL;

    const size_t total_nv = 1 + headers_len;   /* +1 for :status */
    if (total_nv > HTTP2_NV_SCRATCH) {
        nv_heap = emalloc(total_nv * sizeof(nghttp2_nv));
        nv = nv_heap;
    }

    /* :status — three-digit numeric text, always exactly 3 bytes. */
    char status_buf[4];
    snprintf(status_buf, sizeof(status_buf), "%d", status);

    nv[0].name      = (uint8_t *)":status";
    nv[0].namelen   = 7;
    nv[0].value     = (uint8_t *)status_buf;
    nv[0].valuelen  = 3;
    nv[0].flags     = NGHTTP2_NV_FLAG_NONE;

    for (size_t i = 0; i < headers_len; i++) {
        nv[1 + i].name     = (uint8_t *)headers[i].name;
        nv[1 + i].namelen  = headers[i].name_len;
        nv[1 + i].value    = (uint8_t *)headers[i].value;
        nv[1 + i].valuelen = headers[i].value_len;
        nv[1 + i].flags    = NGHTTP2_NV_FLAG_NONE;
    }

    int rc;
    if (body_len == 0) {
        /* No DATA frame — HEADERS with END_STREAM does the whole
         * response (204, 304, HEAD-style). */
        rc = nghttp2_submit_response(session->ng, (int32_t)stream_id,
                                     nv, total_nv, NULL);
    } else {
        nghttp2_data_provider prv;
        prv.source.ptr   = stream;
        prv.read_callback = http2_response_data_read;
        rc = nghttp2_submit_response(session->ng, (int32_t)stream_id,
                                     nv, total_nv, &prv);
    }

    if (nv_heap != NULL) {
        efree(nv_heap);
    }

    return rc == 0 ? 0 : -1;
}

int http2_session_submit_response_streaming(http2_session_t *const session,
                                            const uint32_t stream_id,
                                            const int status,
                                            const http2_header_view_t *const headers,
                                            const size_t headers_len)
{
    if (session == NULL || session->ng == NULL) {
        return -1;
    }
    if (status < 100 || status > 999) {
        return -1;
    }

    http2_stream_t *const stream = http2_session_find_stream(session, stream_id);
    if (stream == NULL) {
        return -1;
    }

    /* Build nghttp2_nv[] same way submit_response does — :status plus
     * caller-supplied headers. HPACK table keeps the bytes alive. */
    nghttp2_nv nv_scratch[HTTP2_NV_SCRATCH];
    nghttp2_nv *nv      = nv_scratch;
    nghttp2_nv *nv_heap = NULL;

    const size_t total_nv = 1 + headers_len;
    if (total_nv > HTTP2_NV_SCRATCH) {
        nv_heap = emalloc(total_nv * sizeof(nghttp2_nv));
        nv = nv_heap;
    }

    char status_buf[4];
    snprintf(status_buf, sizeof(status_buf), "%d", status);

    nv[0].name     = (uint8_t *)":status";
    nv[0].namelen  = 7;
    nv[0].value    = (uint8_t *)status_buf;
    nv[0].valuelen = 3;
    nv[0].flags    = NGHTTP2_NV_FLAG_NONE;

    for (size_t i = 0; i < headers_len; i++) {
        nv[1 + i].name     = (uint8_t *)headers[i].name;
        nv[1 + i].namelen  = headers[i].name_len;
        nv[1 + i].value    = (uint8_t *)headers[i].value;
        nv[1 + i].valuelen = headers[i].value_len;
        nv[1 + i].flags    = NGHTTP2_NV_FLAG_NONE;
    }

    /* Wire the shared data provider — it inspects stream->chunk_queue
     * at every invocation. Initially the queue is empty so the DP
     * will return NGHTTP2_ERR_DEFERRED; nghttp2 then parks the DP
     * until we call nghttp2_session_resume_data after the first
     * chunk append. */
    nghttp2_data_provider prv;
    prv.source.ptr    = stream;
    prv.read_callback = http2_response_data_read;

    const int rc = nghttp2_submit_response(session->ng,
                                           (int32_t)stream_id,
                                           nv, total_nv, &prv);

    if (nv_heap != NULL) { efree(nv_heap); }
    return rc == 0 ? 0 : -1;
}

int http2_session_resume_stream_data(http2_session_t *const session,
                                     const uint32_t stream_id)
{
    if (session == NULL || session->ng == NULL) {
        return -1;
    }
    return nghttp2_session_resume_data(session->ng, (int32_t)stream_id) == 0
               ? 0 : -1;
}

int http2_session_submit_trailer(http2_session_t *const session,
                                 const uint32_t stream_id,
                                 const http2_header_view_t *const trailers,
                                 const size_t trailers_len)
{
    if (session == NULL || session->ng == NULL ||
        trailers == NULL || trailers_len == 0) {
        return -1;
    }

    http2_stream_t *const stream = http2_session_find_stream(session, stream_id);
    if (stream == NULL) {
        return -1;
    }

    /* Flip has_trailers BEFORE queuing — if the data_provider has
     * already been invoked for the final slice (rare but possible
     * under tight scheduling), the NO_END_STREAM flag is missing and
     * nghttp2 will abort the stream. Flipping first closes that
     * window on the remaining drain calls. */
    stream->has_trailers = true;

    nghttp2_nv nv_scratch[HTTP2_NV_SCRATCH];
    nghttp2_nv *nv      = nv_scratch;
    nghttp2_nv *nv_heap = NULL;

    if (trailers_len > HTTP2_NV_SCRATCH) {
        nv_heap = emalloc(trailers_len * sizeof(nghttp2_nv));
        nv = nv_heap;
    }

    for (size_t i = 0; i < trailers_len; i++) {
        nv[i].name     = (uint8_t *)trailers[i].name;
        nv[i].namelen  = trailers[i].name_len;
        nv[i].value    = (uint8_t *)trailers[i].value;
        nv[i].valuelen = trailers[i].value_len;
        nv[i].flags    = NGHTTP2_NV_FLAG_NONE;
    }

    const int rc = nghttp2_submit_trailer(session->ng, (int32_t)stream_id,
                                          nv, trailers_len);
    if (nv_heap != NULL) { efree(nv_heap); }

    if (rc != 0) {
        /* Revert — no trailer got queued, so the data_provider
         * should emit a normal END_STREAM on the final DATA slice. */
        stream->has_trailers = false;
        return -1;
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * Graceful shutdown + PING RTT
 * ------------------------------------------------------------------------- */

int http2_session_terminate(http2_session_t *const session,
                            const uint32_t error_code)
{
    if (session == NULL || session->ng == NULL) {
        return -1;
    }
    /* Graceful variant — `nghttp2_submit_goaway` queues a GOAWAY but
     * LEAVES in-flight streams processing normally (the whole point
     * of graceful shutdown). `nghttp2_session_terminate_session` is
     * the hard variant — it stops processing + makes want_read/write
     * return 0 immediately, which is wrong here: handlers that are
     * mid-flight would lose their response.
     *
     * last_stream_id is the last peer-initiated stream we've accepted
     * and promise to process. nghttp2 exposes it via
     * get_last_proc_stream_id; the peer uses that to decide which of
     * its requests are safe to retry on a new connection. */
    const int32_t last_sid = nghttp2_session_get_last_proc_stream_id(session->ng);
    return nghttp2_submit_goaway(session->ng, NGHTTP2_FLAG_NONE,
                                 last_sid, error_code, NULL, 0) == 0
               ? 0 : -1;
}

int http2_session_submit_ping(http2_session_t *const session)
{
    if (session == NULL || session->ng == NULL) {
        return -1;
    }
    /* Embed send-timestamp in the 8-byte opaque payload. nghttp2
     * copies these bytes verbatim into the frame, and the peer
     * bounces them back in the ACK. Zero need for a side table — the
     * wire carries the state. */
    uint8_t payload[8];
    const uint64_t now_ns = (uint64_t)zend_hrtime();
    memcpy(payload, &now_ns, 8);

    return nghttp2_submit_ping(session->ng, NGHTTP2_FLAG_NONE, payload) == 0
               ? 0 : -1;
}

uint64_t http2_session_last_ping_rtt_ns(const http2_session_t *const session)
{
    return session != NULL ? session->last_ping_rtt_ns : 0;
}

