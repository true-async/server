/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

/* HTTP/2 static-file body delivery FSM.
 *
 * The body is delivered as a producer into the stream's chunk-queue
 * ring — the exact mechanism a PHP streaming handler uses. nghttp2's
 * data_provider is the shared http2_response_data_read; on the h2c
 * plaintext path it announces NGHTTP2_DATA_FLAG_NO_COPY and
 * h2_send_data_callback slices the ring chunks straight into the writev
 * iovec, so file bytes reach the wire with a single userspace copy (the
 * pread itself).
 *
 *   Lifecycle:
 *     - send_static_response builds nghttp2_nv[], allocates the stream
 *       chunk ring, submits the response with the shared data_provider,
 *       registers a persistent dispatch callback on file_io->event,
 *       submits the first async read, and drains the first frames.
 *     - dispatch (file_io completion): finalizes the just-read
 *       zend_string chunk, pushes it onto the ring, eagerly submits the
 *       next read while a ring slot is free (double-buffering: read N+1
 *       overlaps the writev of N), then resume_data + emit.
 *     - When the file is exhausted the FSM sets stream->streaming_ended
 *       so the data_provider flags EOF on the next pull.
 *     - on_window_open (stream->write_event): a drained ring slot or an
 *       inbound WINDOW_UPDATE re-drives the emit and submits the next
 *       read if one was held back by a full ring.
 *     - On stream close the on_close hook disposes file_io, releases
 *       any in-flight chunk, and fires on_done exactly once.
 *
 * Tiering: the read chunk size is min(remaining, H2_STATIC_CHUNK_BYTES),
 * so a small file completes in one read+slurp (one zend_string, one
 * ring slot) while a medium file streams as a handful of 256 KiB chunks
 * with the next read always in flight. The O_DIRECT / page-cache-bypass
 * tier for very large files is a separate follow-up.
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include "php.h"
#include "Zend/zend_async_API.h"
#include "php_http_server.h"
#include "core/http_connection.h"
#include "http2/http2_session.h"
#include "http2/http2_stream.h"
#include "http2/http2_static_response.h"
#include "http_response_internal.h"

#include <nghttp2/nghttp2.h>
#include <errno.h>
#include <stdio.h>
#ifndef PHP_WIN32
# include <unistd.h>
#endif
#include <string.h>

/* Adaptive read-ahead. Instead of a fixed per-stream depth, the FSM
 * targets a byte budget that shrinks as more static streams run
 * concurrently, so a lone large file gets deep read-ahead (the disk
 * runs far ahead of the wire) while hundreds of concurrent files share
 * a bounded total — without this, the read-ahead pinned hundreds of
 * MiB and hit memory_limit.
 *
 *   target = clamp(H2_STATIC_BUDGET_BYTES / active_static_streams,
 *                  H2_STATIC_TARGET_MIN, H2_STATIC_TARGET_MAX)
 *   chunk  = min(H2_STATIC_CHUNK_MAX, target / 2)   (>= 2 chunks fit)
 *
 * The FSM keeps issuing sequential reads (one in flight at a time —
 * the io handle has a single position) as long as the bytes already
 * queued plus the one in flight stay under target. Page-cache-hot
 * reads complete back-to-back, so the ring fills target bytes ahead of
 * the emit loop and emit never starves. */
#define H2_STATIC_BUDGET_BYTES (64u * 1024u * 1024u)   /* per-worker ceiling */
#define H2_STATIC_TARGET_MIN   (128u * 1024u)
#define H2_STATIC_TARGET_MAX   (2u * 1024u * 1024u)
#define H2_STATIC_CHUNK_MAX    (256u * 1024u)

/* Drain-ring slot count. Mirrors H2_CHUNK_RING_SLOTS in
 * http2_strategy.c. target/chunk is at most
 * H2_STATIC_TARGET_MAX / (H2_STATIC_CHUNK_MAX) = 8, so the ring never
 * needs to hold more than this many live entries. */
#define H2_STATIC_RING_SLOTS 8

extern nghttp2_session *http2_session_get_ng(http2_session_t *session);

/* Count of in-progress static-file body streams on this worker. The
 * read-ahead budget is divided across them. Plain long — one reactor
 * per worker process, no cross-thread access. */
static long active_static_streams = 0;

/* Shared data_provider — see http2_session.c. With stream->chunk_queue
 * non-NULL it takes the streaming branch (NO_COPY iov on h2c). */
extern ssize_t http2_static_buffered_data_read(nghttp2_session *,
                                               int32_t, uint8_t *, size_t,
                                               uint32_t *,
                                               nghttp2_data_source *,
                                               void *);

typedef struct {
    http2_stream_t              *stream;
    http2_session_t             *session;
    http_connection_t           *conn;

    zend_async_io_t             *file_io;     /* owned; disposed on finalize */
    uint64_t                     body_offset;
    uint64_t                     body_length;
    uint64_t                     bytes_read;
    bool                         seek_done;
    bool                         eof_reached;

    /* The zend_string the in-flight read is filling. Owned by the FSM
     * until the read completes and it is pushed onto the ring (which
     * then owns it). Released by finalize if a read is interrupted. */
    zend_string                 *pending_chunk;
    bool                         read_in_flight;

    /* Identity guard for spurious dispatch fires. A callback registered
     * during a NOTIFY iteration can re-enter the same iteration with a
     * stale result; matching req identity filters those out. */
    zend_async_io_req_t         *pending_req;
    zend_async_event_callback_t *cb;

    /* Subscribed to stream->write_event. Fires when a ring slot drains
     * or an inbound WINDOW_UPDATE opens our flow-control window. */
    zend_async_event_callback_t *window_cb;

    void                       (*on_done)(void *user, int status);
    void                        *user;
    int                          status;       /* 0 ok, -1 error */
    bool                         done_fired;
} h2_static_state_t;

typedef struct {
    zend_async_event_callback_t base;
    h2_static_state_t          *state;
} h2_static_cb_t;

static void h2_static_finalize(h2_static_state_t *state, int status);
static bool h2_static_submit_read(h2_static_state_t *state);

static void h2_static_cb_dispose(zend_async_event_callback_t *cb,
                                 zend_async_event_t *event)
{
    (void)event;
    efree(cb);
}

/* Per-stream read-ahead target in bytes — the worker budget split
 * across the live static streams, clamped to [MIN, MAX]. */
static size_t h2_static_target_bytes(void)
{
    const long active = active_static_streams > 0 ? active_static_streams : 1;
    size_t target = H2_STATIC_BUDGET_BYTES / (size_t)active;

    if (target < H2_STATIC_TARGET_MIN) { target = H2_STATIC_TARGET_MIN; }

    if (target > H2_STATIC_TARGET_MAX) { target = H2_STATIC_TARGET_MAX; }

    return target;
}

/* True while the FSM should pull another chunk: the bytes already
 * queued in the ring plus the one read in flight are under the
 * adaptive target, and the ring has a physical slot free. Only this
 * FSM produces into the stream's ring, so chunk_queue_bytes is exactly
 * what it has queued and not yet seen drained. */
static bool h2_static_want_more_reads(const h2_static_state_t *state)
{
    const http2_stream_t *stream = state->stream;

    if (stream == NULL || stream->chunk_queue == NULL) {
        return false;
    }

    if (stream->chunk_queue_tail - stream->chunk_queue_head
            >= stream->chunk_queue_cap) {
        return false;
    }

    const size_t outstanding = stream->chunk_queue_bytes
        + (state->read_in_flight && state->pending_chunk != NULL
               ? ZSTR_LEN(state->pending_chunk) : 0u);

    return outstanding < h2_static_target_bytes();
}

/* Push a fully-read chunk onto the stream's drain ring. Compacts first —
 * h2_send_data_callback advances head but never shifts the array. The
 * caller only submits a read when a slot is free and is the sole
 * producer, so this never overflows; the bound is asserted defensively. */
static void h2_static_ring_push(http2_stream_t *stream, zend_string *chunk)
{
    if (stream->chunk_queue_head > 0) {
        const size_t live =
            stream->chunk_queue_tail - stream->chunk_queue_head;

        if (live > 0) {
            memmove(stream->chunk_queue,
                    stream->chunk_queue + stream->chunk_queue_head,
                    live * sizeof(zend_string *));
        }

        stream->chunk_queue_head = 0;
        stream->chunk_queue_tail = live;
    }

    ZEND_ASSERT(stream->chunk_queue_tail < stream->chunk_queue_cap);

    stream->chunk_queue[stream->chunk_queue_tail++] = chunk;
    stream->chunk_queue_bytes += ZSTR_LEN(chunk);
}

static void h2_static_finalize(h2_static_state_t *state, const int status)
{
    if (state->cb != NULL && state->file_io != NULL) {
        (void)state->file_io->event.del_callback(&state->file_io->event,
                                                 state->cb);
        state->cb = NULL;
    }

    if (state->window_cb != NULL
        && state->stream != NULL
        && state->stream->write_event != NULL) {
        zend_async_event_t *we = &((zend_async_trigger_event_t *)
                                   state->stream->write_event)->base;
        (void)we->del_callback(we, state->window_cb);
        state->window_cb = NULL;
    }

    if (state->file_io != NULL) {
        if (state->file_io->event.dispose != NULL) {
            state->file_io->event.dispose(&state->file_io->event);
        }

        state->file_io = NULL;
    }

    if (state->pending_chunk != NULL) {
        zend_string_release(state->pending_chunk);
        state->pending_chunk = NULL;
    }

    void (*on_done)(void *, int) = state->on_done;
    void *user = state->user;
    const bool fire = !state->done_fired && on_done != NULL;
    state->done_fired = true;

    if (state->stream != NULL) {
        if (state->stream->on_close_user == state) {
            state->stream->on_close = NULL;
            state->stream->on_close_user = NULL;
        }

        state->stream = NULL;
    }

    efree(state);

    active_static_streams--;

    if (fire) {
        on_done(user, status);
    }
}

/* Stream-close hook. Fires from cb_on_stream_close after nghttp2 is
 * done with the stream. Either DATA frames drained (NO_ERROR) or the
 * peer reset the stream (non-zero). Dispose file_io + fire on_done. */
static void h2_static_on_stream_close(void *user, const uint32_t error_code)
{
    h2_static_state_t *state = (h2_static_state_t *)user;
    const int status = (error_code == NGHTTP2_NO_ERROR && state->status == 0)
                           ? 0 : -1;
    h2_static_finalize(state, status);
}

/* Mark the body exhausted: the data_provider sees an empty ring +
 * streaming_ended and flags EOF on its next pull. */
static void h2_static_mark_ended(h2_static_state_t *state)
{
    state->eof_reached = true;

    if (state->stream != NULL) {
        state->stream->streaming_ended = true;
    }
}

/* Drive nghttp2: resume the deferred data_provider and run one emit so
 * the freshly-available DATA frames leave the wire. */
static void h2_static_kick(h2_static_state_t *state)
{
    if (state->session == NULL || state->stream == NULL
        || state->stream->peer_closed) {
        return;
    }

    nghttp2_session *ng = http2_session_get_ng(state->session);

    if (ng == NULL) {
        return;
    }

    (void)nghttp2_session_resume_data(ng, (int32_t)state->stream->stream_id);

    if (state->conn != NULL) {
        http2_session_emit(state->session);
    }
}

/* Trigger-event subscriber on stream->write_event. Fires when
 * h2_send_data_callback drains a ring slot or cb_on_frame_recv sees an
 * inbound WINDOW_UPDATE. Re-drive the emit, and submit the next read if
 * one was held back by a full ring. Safe on stray triggers — both steps
 * are idempotent. */
static void h2_static_on_window_open(zend_async_event_t *event,
                                     zend_async_event_callback_t *callback,
                                     void *result,
                                     zend_object *exception)
{
    (void)event;
    (void)result;
    (void)exception;

    h2_static_state_t *state = ((h2_static_cb_t *)callback)->state;

    if (state->stream == NULL || state->stream->peer_closed) {
        return;
    }

    if (!state->read_in_flight && !state->eof_reached
        && h2_static_want_more_reads(state)) {
        if (UNEXPECTED(!h2_static_submit_read(state))) {
            state->status = -1;
            h2_static_mark_ended(state);
        }
    }

    if (state->session != NULL && state->conn != NULL) {
        http2_session_emit(state->session);
    }
}

/* file_io completion dispatch. Identity-guarded against stale fires.
 * On bytes: finalizes the chunk, pushes it onto the ring, eagerly
 * submits the next read while a slot is free, then resume + emit. On
 * error / short read: marks the body ended so the provider flags EOF. */
static void h2_static_dispatch(zend_async_event_t *event,
                               zend_async_event_callback_t *callback,
                               void *result,
                               zend_object *exception)
{
    (void)event;
    h2_static_state_t *state = ((h2_static_cb_t *)callback)->state;
    zend_async_io_req_t *req = (zend_async_io_req_t *)result;

    /* Spurious-fire guard — see the long note in the previous revision:
     * the notify iteration can re-enter our cb with a stale, already-
     * disposed req. `req == pending_req` alone is not enough since the
     * freed address may be reused; gate on completed too. */
    if (req == NULL || req != state->pending_req || !req->completed) {
        return;
    }

    state->pending_req = NULL;
    state->read_in_flight = false;

    const ssize_t transferred = (ssize_t)req->transferred;
    const bool err = (exception != NULL || req->exception != NULL);

    if (req->exception != NULL) {
        OBJ_RELEASE(req->exception);
        req->exception = NULL;
    }

    if (req->dispose != NULL) {
        req->dispose(req);
    }

    zend_string *chunk = state->pending_chunk;
    state->pending_chunk = NULL;

    if (UNEXPECTED(err) || transferred <= 0) {
        /* Read error or unexpected EOF before body_length — truncate
         * the response gracefully; the connection survives. */
        if (chunk != NULL) {
            zend_string_release(chunk);
        }

        if (err || transferred < 0) {
            state->status = -1;
        }

        h2_static_mark_ended(state);
        h2_static_kick(state);
        return;
    }

    /* Finalize the chunk to the bytes actually read — it was allocated
     * at the requested size, which a short read leaves over-long. */
    ZSTR_LEN(chunk) = (size_t)transferred;
    ZSTR_VAL(chunk)[transferred] = '\0';
    state->bytes_read += (uint64_t)transferred;

    if (state->stream == NULL || state->stream->peer_closed) {
        zend_string_release(chunk);
        return;
    }

    h2_static_ring_push(state->stream, chunk);

    if (state->bytes_read >= state->body_length) {
        h2_static_mark_ended(state);
    } else if (h2_static_want_more_reads(state)) {
        /* Double-buffer: get the next read in flight before this
         * chunk's writev so the file read overlaps the wire write. */
        if (UNEXPECTED(!h2_static_submit_read(state))) {
            state->status = -1;
            h2_static_mark_ended(state);
        }
    }

    h2_static_kick(state);
}

/* Submit the next ZEND_ASYNC_IO_READ into a freshly allocated chunk.
 * Reads are sequential — the fd position advances on its own, so the
 * range offset is applied once via SEEK before the first read. Sets
 * read_in_flight on success. Returns false on submit failure — caller
 * marks the body ended. */
static bool h2_static_submit_read(h2_static_state_t *state)
{
    if (state->file_io == NULL) {
        return false;
    }

    const uint64_t remaining = state->body_length > state->bytes_read
                                   ? state->body_length - state->bytes_read
                                   : 0;

    if (remaining == 0) {
        h2_static_mark_ended(state);
        return true;
    }

    if (!state->seek_done) {
        if (state->body_offset != 0) {
            (void)ZEND_ASYNC_IO_SEEK(state->file_io,
                                     (zend_off_t)state->body_offset,
                                     SEEK_SET);
        }

        state->seek_done = true;
    }

    /* Chunk size = half the adaptive target, capped. */
    size_t chunk_sz = h2_static_target_bytes() / 2u;

    if (chunk_sz > H2_STATIC_CHUNK_MAX) { chunk_sz = H2_STATIC_CHUNK_MAX; }

    const size_t want = remaining < chunk_sz
                            ? (size_t)remaining
                            : chunk_sz;

    zend_string *chunk = zend_string_alloc(want, 0);

    state->pending_req = ZEND_ASYNC_IO_READ(state->file_io,
                                            ZSTR_VAL(chunk), want);

    if (state->pending_req == NULL) {
        zend_string_release(chunk);
        return false;
    }

    state->pending_chunk = chunk;
    state->read_in_flight = true;
    return true;
}

/* Build nghttp2_nv[] (with leading :status) from response_obj. Returns
 * the heap allocation pointer (NULL if scratch was sufficient); the
 * filled view is written through *out_nv / *out_count. */
static nghttp2_nv *h2_static_build_nv(zend_object *response_obj,
                                      const int status,
                                      char *status_buf3,
                                      nghttp2_nv *scratch,
                                      const size_t scratch_cap,
                                      size_t *out_count,
                                      nghttp2_nv **out_nv)
{
    HashTable *headers = http_response_get_headers_table(response_obj);

    size_t total_values = 1;

    if (headers != NULL) {
        zend_string *name;
        zval        *values;
        ZEND_HASH_FOREACH_STR_KEY_VAL(headers, name, values) {
            if (name == NULL)                                              continue;

            if (!http_response_header_allowed_h2h3(ZSTR_VAL(name), ZSTR_LEN(name))) continue;

            if (EXPECTED(Z_TYPE_P(values) == IS_STRING)) {
                total_values++;
            } else if (Z_TYPE_P(values) == IS_ARRAY) {
                zval *val;
                ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(values), val) {
                    if (Z_TYPE_P(val) == IS_STRING) { total_values++; }
                } ZEND_HASH_FOREACH_END();
            }
        } ZEND_HASH_FOREACH_END();
    }

    nghttp2_nv *nv = scratch;
    nghttp2_nv *nv_heap = NULL;

    if (total_values > scratch_cap) {
        nv_heap = emalloc(total_values * sizeof(nghttp2_nv));
        nv = nv_heap;
    }

    const int s = (status >= 100 && status <= 999) ? status : 200;
    /* Hand-format three digits — :status is always exactly 3 bytes
     * per RFC 9113 §8.3.1. snprintf("%03d") works but pulls locale +
     * format-string parsing on every response. */
    status_buf3[0] = (char)('0' + (s / 100));
    status_buf3[1] = (char)('0' + ((s / 10) % 10));
    status_buf3[2] = (char)('0' + (s % 10));
    status_buf3[3] = '\0';

    nv[0].name     = (uint8_t *)":status";
    nv[0].namelen  = 7;
    nv[0].value    = (uint8_t *)status_buf3;
    nv[0].valuelen = 3;
    nv[0].flags    = NGHTTP2_NV_FLAG_NONE;

    size_t i = 1;

    if (headers != NULL) {
        zend_string *name;
        zval        *values;
        ZEND_HASH_FOREACH_STR_KEY_VAL(headers, name, values) {
            if (name == NULL)                                              continue;

            if (!http_response_header_allowed_h2h3(ZSTR_VAL(name), ZSTR_LEN(name))) continue;

            if (EXPECTED(Z_TYPE_P(values) == IS_STRING)) {
                nv[i].name     = (uint8_t *)ZSTR_VAL(name);
                nv[i].namelen  = ZSTR_LEN(name);
                nv[i].value    = (uint8_t *)Z_STRVAL_P(values);
                nv[i].valuelen = Z_STRLEN_P(values);
                nv[i].flags    = NGHTTP2_NV_FLAG_NONE;
                i++;
            } else if (Z_TYPE_P(values) == IS_ARRAY) {
                zval *val;
                ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(values), val) {
                    if (Z_TYPE_P(val) != IS_STRING) { continue; }
                    nv[i].name     = (uint8_t *)ZSTR_VAL(name);
                    nv[i].namelen  = ZSTR_LEN(name);
                    nv[i].value    = (uint8_t *)Z_STRVAL_P(val);
                    nv[i].valuelen = Z_STRLEN_P(val);
                    nv[i].flags    = NGHTTP2_NV_FLAG_NONE;
                    i++;
                } ZEND_HASH_FOREACH_END();
            }
        } ZEND_HASH_FOREACH_END();
    }

    *out_count = i;
    *out_nv    = nv;
    return nv_heap;
}

int h2_stream_send_static_response(void *ctx,
                                   zend_object *response_obj,
                                   zend_async_io_t *file_io,
                                   const uint64_t body_offset,
                                   const uint64_t body_length,
                                   const bool head_only_in,
                                   void (*on_done)(void *user, int status),
                                   void *user)
{
    http2_stream_t *stream = (http2_stream_t *)ctx;

    if (UNEXPECTED(stream == NULL || stream->session == NULL)) {
        return -1;
    }

    http_connection_t *conn = http2_session_get_conn(stream->session);

    if (UNEXPECTED(conn == NULL)) {
        return -1;
    }

    nghttp2_session *ng = http2_session_get_ng(stream->session);

    if (UNEXPECTED(ng == NULL)) {
        return -1;
    }

    /* head_only is implied by file_io == NULL (inline-body / 304 / HEAD). */
    const bool head_only = head_only_in || file_io == NULL;

    const int status_code = http_response_get_status_code(response_obj);
    nghttp2_nv scratch[64];
    nghttp2_nv *nv = NULL;
    size_t nv_count = 0;
    char status_buf[4];
    nghttp2_nv *nv_heap = h2_static_build_nv(response_obj, status_code,
                                                   status_buf, scratch, 64,
                                                   &nv_count, &nv);

    h2_static_state_t *state = NULL;
    int rc = -1;

    if (head_only) {
        /* HEAD with file_io ≠ NULL: caller transferred ownership but
         * we have no body to push — dispose the fd here. */
        if (file_io != NULL && file_io->event.dispose != NULL) {
            file_io->event.dispose(&file_io->event);
        }

        zend_string *inline_body = http_response_get_body_string(response_obj);
        const bool has_inline =
            inline_body != NULL && ZSTR_LEN(inline_body) > 0
            && status_code != 304 && status_code != 204;

        if (has_inline) {
            /* Pin the inline body on the stream so the buffered
             * data_provider serves it. Response zval lifetime brackets
             * the stream — pointer stable for the duration of the drain. */
            stream->response_body        = ZSTR_VAL(inline_body);
            stream->response_body_len    = ZSTR_LEN(inline_body);
            stream->response_body_offset = 0;

            nghttp2_data_provider prv;
            prv.source.ptr   = stream;
            prv.read_callback = http2_static_buffered_data_read;
            rc = nghttp2_submit_response(ng, (int32_t)stream->stream_id,
                                         nv, nv_count, &prv);
        } else {
            /* No body — HEADERS with END_STREAM does the whole job. */
            rc = nghttp2_submit_response(ng, (int32_t)stream->stream_id,
                                         nv, nv_count, NULL);
        }
    } else {
        /* Body branch: allocate FSM state + the stream's drain ring,
         * install the close hook, register the persistent dispatch cb
         * on file_io, register the shared data_provider (it takes the
         * streaming branch because chunk_queue is now non-NULL), and
         * submit the first read. */
        state = ecalloc(1, sizeof(*state));
        active_static_streams++;
        state->stream      = stream;
        state->session     = stream->session;
        state->conn        = conn;
        state->file_io     = file_io;
        state->body_offset = body_offset;
        state->body_length = body_length;
        state->on_done     = on_done;
        state->user        = user;

        if (stream->chunk_queue == NULL) {
            stream->chunk_queue_cap   = H2_STATIC_RING_SLOTS;
            stream->chunk_queue       = ecalloc(stream->chunk_queue_cap,
                                                sizeof(zend_string *));
            stream->chunk_queue_head  = 0;
            stream->chunk_queue_tail  = 0;
            stream->chunk_queue_bytes = 0;
            stream->chunk_read_offset = 0;
        }

        h2_static_cb_t *cb = (h2_static_cb_t *)ZEND_ASYNC_EVENT_CALLBACK_EX(
            h2_static_dispatch, sizeof(h2_static_cb_t));

        if (UNEXPECTED(cb == NULL)) {
            if (nv_heap != NULL) { efree(nv_heap); }
            /* state owns file_io; finalize disposes + fires on_done(-1). */
            h2_static_finalize(state, -1);
            return 0;
        }

        cb->base.dispose = h2_static_cb_dispose;
        cb->state        = state;

        if (UNEXPECTED(!file_io->event.add_callback(&file_io->event, &cb->base))) {
            efree(cb);

            if (nv_heap != NULL) { efree(nv_heap); }

            h2_static_finalize(state, -1);
            return 0;
        }

        state->cb = &cb->base;

        /* Subscribe to stream->write_event so a drained ring slot or an
         * inbound WINDOW_UPDATE re-drives our drain. The event is lazy
         * on the stream — create it here. Failure leaves window_cb NULL;
         * we lose the slot-drain wake but the request can still finish
         * as long as reads keep the ring fed. */
        if (stream->write_event == NULL) {
            stream->write_event = ZEND_ASYNC_NEW_TRIGGER_EVENT();
        }

        if (stream->write_event != NULL) {
            h2_static_cb_t *wcb = (h2_static_cb_t *)ZEND_ASYNC_EVENT_CALLBACK_EX(
                h2_static_on_window_open, sizeof(h2_static_cb_t));

            if (wcb != NULL) {
                wcb->base.dispose = h2_static_cb_dispose;
                wcb->state        = state;
                zend_async_event_t *we = &((zend_async_trigger_event_t *)
                                           stream->write_event)->base;

                if (we->add_callback(we, &wcb->base)) {
                    state->window_cb = &wcb->base;
                } else {
                    efree(wcb);
                }
            }
        }

        stream->on_close      = h2_static_on_stream_close;
        stream->on_close_user = state;

        nghttp2_data_provider prv;
        prv.source.ptr   = stream;
        prv.read_callback = http2_static_buffered_data_read;
        rc = nghttp2_submit_response(ng, (int32_t)stream->stream_id,
                                     nv, nv_count, &prv);

        if (rc == 0 && UNEXPECTED(!h2_static_submit_read(state))) {
            /* Submit failed before any read — no body bytes will ever
             * arrive. Mark ended so the provider flags EOF and the
             * stream closes through nghttp2's normal path. */
            state->status = -1;
            h2_static_mark_ended(state);
        }
    }

    if (nv_heap != NULL) {
        efree(nv_heap);
    }

    if (UNEXPECTED(rc != 0)) {
        if (state != NULL) {
            stream->on_close      = NULL;
            stream->on_close_user = NULL;
            /* state owns file_io; finalize disposes + fires on_done(-1). */
            h2_static_finalize(state, -1);
            return 0;
        }
        /* head_only: file_io already disposed above. Per the vtable
         * contract, rc != 0 means on_done WILL NOT fire and the
         * caller still owns file_io — but we just consumed it. The
         * static FSM finalize fires its own on_static_done(-1) on
         * this branch, so the pinned conn / counters still drain. */
        return -1;
    }

    /* Push HEADERS + the first DATA window onto the wire. Subsequent
     * WINDOW_UPDATE / inbound traffic drives further drains via
     * http2_feed's tail. */
    http2_session_emit(stream->session);

    /* Head-only completes synchronously — there's no asynchronous
     * body callback to wait for, and the caller's static FSM tail
     * (counters, finalize, keep-alive verdict) needs on_done now to
     * release its conn pin. The stream itself still closes through
     * nghttp2's normal path; we don't re-register a close hook on
     * this branch because there's no file_io to dispose. */
    if (head_only && on_done != NULL) {
        on_done(user, 0);
    }

    return 0;
}
