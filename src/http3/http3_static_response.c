/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

/* HTTP/3 static-file body delivery FSM.
 *
 * Wired into h3_stream_ops.send_static_response so the file-delivery
 * engine in src/send_file.c can hand a stream off to us once it has
 * decided headers + range + open file_io.
 *
 * Transport: read the file slice in 16 KiB chunks via ZEND_ASYNC_IO_READ,
 * wrap each into a zend_string, push it through h3_stream_append_chunk —
 * that hooks the existing streaming machinery (chunk_queue + nghttp3
 * data_reader + acked_stream_data release + window-extend backpressure).
 * When the slice is done, h3_stream_mark_ended flags EOF on the data_reader.
 *
 * Callback-driven, never a coroutine — this is what lets the same pump run on
 * a transport reactor thread, where the reactor/worker split serves static
 * mounts and marshalled sendFile()s and no PHP coroutine can exist. Reads
 * complete into h3_static_read_dispatch, which pushes the chunk and decides
 * whether to read on. Two things stop the read-ahead, and each has a wake:
 *   - the stream's unsent bytes reach the high-water mark → wait on
 *     write_event (fired by MAX_STREAM_DATA and by the peer's ACKs);
 *   - the per-thread static-memory budget is exhausted → park on the throttled
 *     list, which h3_static_account_debit drains once usage falls back.
 * Connection teardown cancels an in-flight pump via h3_static_cancel — with no
 * coroutine there is no scope to cancel it for us.
 *
 * Why no NO_COPY/sendfile fast path on H/3:
 *   QUIC always encrypts. Kernel sendfile(2) cannot splice plaintext
 *   into an encrypted stream; only kTLS+sendfile is the known kernel
 *   path, and it doesn't extend to QUIC. nghttp3's data_reader API
 *   has no zero-copy hook either. Same trade-off applies on H/2 (see
 *   src/http2/http2_static_response.c top-of-file comment for the
 *   nghttp2 NO_COPY discussion + references).
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include "php.h"
#include "Zend/zend_async_API.h"
#include "Zend/zend_exceptions.h"   /* zend_clear_exception */
#include "php_http_server.h"
#include "core/http_connection.h"
#include "core/http_connection_internal.h"
#include "http3_internal.h"
#include "http3_listener.h"
#include "http3/http3_stream.h"

#include <errno.h>
#include <string.h>

#define H3_STATIC_READ_CHUNK_BYTES (16u * 1024u)

/* Read-ahead ceiling per stream: stop pulling from disk once this many bytes
 * are queued but not yet handed to nghttp3. Four chunks keeps the socket fed
 * across a read round-trip without letting a slow peer grow the queue. */
#define H3_STATIC_STREAM_HIGH_WATER (4u * H3_STATIC_READ_CHUNK_BYTES)

/* Per-thread static-delivery memory cap across concurrent streams.
 * Formula and clamps mirror http2_static_response.c. */
#define H3_STATIC_BUDGET_FLOOR      (4u * 1024u * 1024u)
#define H3_STATIC_BUDGET_FALLBACK   (64u * 1024u * 1024u)  /* memory_limit = -1 */
#define H3_STATIC_BUDGET_RESERVE_FRAC 8u                   /* hard top = 87.5% */
#define H3_STATIC_RESUME_NUM        4u
#define H3_STATIC_RESUME_DEN        5u                     /* resume below 80% */

/* 0 = not yet initialised (h3_static_budget_init_once). */
ZEND_TLS size_t h3_static_budget_bytes           = 0;
ZEND_TLS size_t h3_static_global_bytes_in_flight = 0;
ZEND_TLS size_t h3_static_global_high_water      = 0;
ZEND_TLS bool   h3_static_global_throttled       = false;

typedef struct h3_static_state_s h3_static_state_t;

struct h3_static_state_s {
    http3_stream_t              *stream;

    zend_async_io_t             *file_io;
    uint64_t                     body_offset;
    uint64_t                     body_length;
    uint64_t                     bytes_sent;

    /* The chunk the in-flight read is filling; owned until pushed. */
    zend_string                 *pending_chunk;
    /* Identity guard against a stale NOTIFY on a reused req address. */
    zend_async_io_req_t         *pending_req;
    zend_async_event_callback_t *cb;          /* on file_io->event */
    zend_async_event_callback_t *window_cb;   /* on stream->write_event */

    void                       (*on_done)(void *user, int status);
    void                        *user;
    int                          status;      /* 0 ok, -1 error */
    bool                         done_fired;
    bool                         seek_done;
    bool                         read_in_flight;
    bool                         eof_reached;   /* whole slice queued */
    /* Set while a push/EOF of ours is inside the connection drain. That drain
     * can deliver an ACK, which fires write_event, which re-enters try_read —
     * on a state the outer call is still driving. The nested call must do
     * nothing, or it finishes (and frees) the pump under its own caller. */
    bool                         busy;

    /* Per-thread list of pumps waiting on the global cap; the debit path
     * drains it once usage crosses the resume threshold. */
    h3_static_state_t           *throttled_prev;
    h3_static_state_t           *throttled_next;
    bool                         in_throttled_list;
};

ZEND_TLS h3_static_state_t *h3_static_throttled_head = NULL;

typedef struct {
    zend_async_event_callback_t base;
    h3_static_state_t          *state;
} h3_static_cb_t;

static void h3_static_finalize(h3_static_state_t *state);
static void h3_static_fail(h3_static_state_t *state);
static void h3_static_try_read(h3_static_state_t *state);
static void h3_static_throttle_unlink(h3_static_state_t *state);

static void h3_static_budget_init_once(void)
{
    if (h3_static_budget_bytes != 0) {
        return;
    }

    const zend_long limit = PG(memory_limit);
    size_t budget;

    if (limit <= 0) {
        budget = H3_STATIC_BUDGET_FALLBACK;
    } else {
        const size_t mem = (size_t)limit;
        const size_t reserve = mem / H3_STATIC_BUDGET_RESERVE_FRAC;
        const size_t hard_top = mem > reserve ? mem - reserve : mem;

        budget = mem / 4u;
        if (budget > hard_top) { budget = hard_top; }
    }

    if (budget < H3_STATIC_BUDGET_FLOOR) { budget = H3_STATIC_BUDGET_FLOOR; }

    h3_static_budget_bytes = budget;
}

void h3_static_account_alloc(const size_t n)
{
    h3_static_global_bytes_in_flight += n;

    if (h3_static_global_bytes_in_flight > h3_static_global_high_water) {
        h3_static_global_high_water = h3_static_global_bytes_in_flight;
    }
}

static bool h3_static_over_budget(void)
{
    h3_static_budget_init_once();
    return h3_static_global_bytes_in_flight >= h3_static_budget_bytes;
}

/* Resume every parked pump. try_read re-checks the budget, so a pump that
 * finds it exhausted again simply parks back. */
static void h3_static_throttle_kick_all(void)
{
    h3_static_state_t *state = h3_static_throttled_head;

    while (state != NULL) {
        h3_static_state_t *const next = state->throttled_next;

        h3_static_throttle_unlink(state);
        h3_static_try_read(state);   /* may finalize + free `state` */

        state = next;
    }
}

void h3_static_account_debit(const size_t n)
{
    /* Clamp: the one-shot teardown remainder can overlap chunks already
     * debited on the ACK path if a release raced, so never underflow. */
    h3_static_global_bytes_in_flight -=
        (n <= h3_static_global_bytes_in_flight) ? n : h3_static_global_bytes_in_flight;

    /* hysteresis: wake every parked pump once usage falls below 80% */
    if (h3_static_global_throttled
        && h3_static_global_bytes_in_flight
               < h3_static_budget_bytes * H3_STATIC_RESUME_NUM
                                        / H3_STATIC_RESUME_DEN) {
        h3_static_global_throttled = false;
        h3_static_throttle_kick_all();
    }
}

static void h3_static_throttle_park(h3_static_state_t *state)
{
    h3_static_global_throttled = true;

    if (state->in_throttled_list) {
        return;
    }

    state->throttled_prev = NULL;
    state->throttled_next = h3_static_throttled_head;

    if (h3_static_throttled_head != NULL) {
        h3_static_throttled_head->throttled_prev = state;
    }

    h3_static_throttled_head = state;
    state->in_throttled_list = true;
}

static void h3_static_throttle_unlink(h3_static_state_t *state)
{
    if (!state->in_throttled_list) {
        return;
    }

    if (state->throttled_prev != NULL) {
        state->throttled_prev->throttled_next = state->throttled_next;
    } else {
        h3_static_throttled_head = state->throttled_next;
    }

    if (state->throttled_next != NULL) {
        state->throttled_next->throttled_prev = state->throttled_prev;
    }

    state->throttled_prev    = NULL;
    state->throttled_next    = NULL;
    state->in_throttled_list = false;
}

static void h3_static_cb_dispose(zend_async_event_callback_t *cb,
                                 zend_async_event_t *event)
{
    (void)event;
    efree(cb);
}

static void h3_static_finalize(h3_static_state_t *state)
{
    if (state->done_fired) {
        return;
    }

    /* Claim it up front: everything below can re-enter through a drain. */
    state->done_fired = true;

    h3_static_throttle_unlink(state);

    if (state->stream != NULL && state->stream->static_body_state == state) {
        state->stream->static_body_state = NULL;
    }

    if (state->cb != NULL && state->file_io != NULL) {
        (void)state->file_io->event.del_callback(&state->file_io->event,
                                                 state->cb);
        state->cb = NULL;
    }

    if (state->window_cb != NULL
        && state->stream != NULL
        && state->stream->write_event != NULL) {
        zend_async_event_t *const we = &state->stream->write_event->base;
        (void)we->del_callback(we, state->window_cb);
        state->window_cb = NULL;
    }

    if (state->pending_chunk != NULL) {
        /* Never pushed, so never charged to the budget — just drop it. */
        zend_string_release(state->pending_chunk);
        state->pending_chunk = NULL;
    }

    if (state->file_io != NULL) {
        if (state->file_io->event.dispose != NULL) {
            state->file_io->event.dispose(&state->file_io->event);
        }

        state->file_io = NULL;
    }

    void (*on_done)(void *, int) = state->on_done;
    void *const user = state->user;
    const int status = state->status;
    const bool fire = on_done != NULL;

    efree(state);

    if (fire) {
        on_done(user, status);
    }
}

/* Give up on the body mid-flight — the file was truncated under us, or the read
 * failed. RESET the write side instead of flagging a clean EOF: the status and
 * the Content-Length are already on the wire, so ending the stream normally
 * would hand the peer a short body under a success status and call it done. A
 * reset is how HTTP/3 says "this transfer failed".
 *
 * The teardown itself waits for the stream close the reset triggers — releasing
 * the file io from inside its own completion callback would free it while
 * nghttp3 still owns every queued chunk. */
static void h3_static_fail(h3_static_state_t *state)
{
    state->status = -1;

    http3_stream_t *const s = state->stream;
    http3_connection_t *const c = (s != NULL) ? s->conn : NULL;

    if (c != NULL && !s->peer_closed && !state->eof_reached) {
        state->eof_reached = true;   /* nothing more will be read or queued */

        if (c->ngtcp2_conn != NULL) {
            (void)ngtcp2_conn_shutdown_stream_write(
                (ngtcp2_conn *)c->ngtcp2_conn, 0, s->stream_id,
                NGHTTP3_H3_INTERNAL_ERROR);
            http3_listener_mark_flush(c->listener, c);
            http3_listener_queue_epilogue_flush(c->listener);
            return;   /* close_cb → h3_static_stream_closed → finalize */
        }
    }

    h3_static_finalize(state);
}

/* nghttp3 closed the stream: every chunk has been sent or dropped, so the pump
 * can retire. This is the only success path — the read callback never tears
 * itself down. */
void h3_static_stream_closed(http3_stream_t *s)
{
    if (s == NULL || s->static_body_state == NULL) {
        return;
    }

    h3_static_state_t *const state = (h3_static_state_t *)s->static_body_state;

    if (!state->eof_reached || state->bytes_sent < state->body_length) {
        state->status = -1;   /* closed before the slice was fully queued */
    }

    h3_static_finalize(state);
}

void h3_static_cancel(http3_stream_t *s)
{
    if (s == NULL || s->static_body_state == NULL) {
        return;
    }

    h3_static_state_t *const state = (h3_static_state_t *)s->static_body_state;

    state->status = -1;
    h3_static_finalize(state);
}

/* Range offset is applied via SEEK before the first read (the fd is read
 * sequentially from there on). */
static bool h3_static_submit_read(h3_static_state_t *state)
{
    if (state->file_io == NULL) {
        return false;
    }

    if (!state->seek_done) {
        if (state->body_offset != 0
            && ZEND_ASYNC_IO_SEEK(state->file_io, (zend_off_t)state->body_offset,
                                  SEEK_SET) < 0) {
            return false;
        }

        state->seek_done = true;
    }

    const uint64_t remaining = state->body_length - state->bytes_sent;
    const size_t want = remaining < H3_STATIC_READ_CHUNK_BYTES
                            ? (size_t)remaining
                            : H3_STATIC_READ_CHUNK_BYTES;

    /* Persistent, not ZMM: on the reactor/worker split this pump runs on the
     * transport thread, and the chunk outlives the push — nghttp3 holds it for
     * retransmit until the peer ACKs. The same discipline the worker follows
     * when it hands stream chunks to a reactor (worker_dispatch.c). */
    zend_string *const chunk = zend_string_alloc(want, 1);

    /* Store before submitting: a failure path below (and finalize) reclaims
     * the chunk through state->pending_chunk. */
    state->pending_chunk = chunk;
    state->pending_req   = ZEND_ASYNC_IO_READ(state->file_io, ZSTR_VAL(chunk), want);

    if (UNEXPECTED(state->pending_req == NULL)) {
        /* Submit failure leaves an exception behind; there is no coroutine to
         * unwind it here, so absorb it and report through on_done(-1). */
        if (EG(exception) != NULL) {
            zend_clear_exception();
        }

        state->pending_chunk = NULL;
        zend_string_release(chunk);
        return false;
    }

    state->read_in_flight = true;
    return true;
}

/* The single decision point: finish, park, or read on. */
static void h3_static_try_read(h3_static_state_t *state)
{
    if (state->busy || state->done_fired) {
        return;
    }

    http3_stream_t *const s = state->stream;

    if (UNEXPECTED(s == NULL || s->conn == NULL || s->peer_closed)) {
        h3_static_fail(state);
        return;
    }

    if (state->read_in_flight) {
        return;
    }

    if (state->bytes_sent >= state->body_length) {
        /* Slice queued. Do NOT tear down here: we are inside the file io's own
         * completion callback, and nghttp3 still holds every queued chunk until
         * the peer ACKs it. Flag EOF on the data_reader and let the stream close
         * finish us — the same lifecycle H2's static pump follows. */
        state->eof_reached = true;
        state->busy = true;   /* mark_ended drains — it must not re-enter us */
        h3_stream_mark_ended(s);
        state->busy = false;
        return;
    }

    if (h3_static_over_budget()) {
        h3_static_throttle_park(state);
        return;
    }

    /* Read-ahead ceiling: the write_event wake (ACK / MAX_STREAM_DATA) brings
     * us back once the queue has drained. */
    if (s->chunk_pending_bytes >= H3_STATIC_STREAM_HIGH_WATER) {
        return;
    }

    if (UNEXPECTED(!h3_static_submit_read(state))) {
        h3_static_fail(state);
    }
}

static void h3_static_read_dispatch(zend_async_event_t *event,
                                    zend_async_event_callback_t *callback,
                                    void *result,
                                    zend_object *exception)
{
    (void)event;
    h3_static_state_t *const state = ((h3_static_cb_t *)callback)->state;
    zend_async_io_req_t *const req = (zend_async_io_req_t *)result;

    /* Spurious-fire guard: a freed req address may be reused — gate on the
     * identity we submitted plus `completed`. */
    if (req == NULL || req != state->pending_req || !req->completed) {
        return;
    }

    state->pending_req    = NULL;
    state->read_in_flight = false;

    const ssize_t transferred = (ssize_t)req->transferred;
    const bool    err = (exception != NULL || req->exception != NULL);

    if (req->exception != NULL) {
        OBJ_RELEASE(req->exception);
        req->exception = NULL;
    }

    if (req->dispose != NULL) {
        req->dispose(req);
    }

    zend_string *const chunk = state->pending_chunk;
    state->pending_chunk = NULL;

    /* transferred == 0 is EOF before the slice end — the file was truncated
     * under us. The bytes already on the wire belong to the peer; report the
     * failure and let the on_done policy retire the stream. */
    if (UNEXPECTED(err || transferred <= 0 || chunk == NULL)) {
        if (chunk != NULL) {
            zend_string_release(chunk);
        }

        h3_static_fail(state);
        return;
    }

    /* Shrink to the bytes actually read — the alloc was sized for the request. */
    ZSTR_LEN(chunk) = (size_t)transferred;
    ZSTR_VAL(chunk)[transferred] = '\0';
    state->bytes_sent += (uint64_t)transferred;

    /* Takes the ref; submits headers on the first call, then queues + resumes
     * nghttp3 and drains. It never suspends: s->static_body_state is set, which
     * is what tells append_chunk this producer does its own backpressure. */
    state->busy = true;
    const int rc = h3_stream_append_chunk(state->stream, chunk);
    state->busy = false;

    if (UNEXPECTED(rc != HTTP_STREAM_APPEND_OK)) {
        h3_static_fail(state);
        return;
    }

    h3_static_try_read(state);
}

/* Peer ACKed / extended the window (and mark_peer_closed fires it too, which
 * try_read turns into a clean failure). */
static void h3_static_window_dispatch(zend_async_event_t *event,
                                      zend_async_event_callback_t *callback,
                                      void *result,
                                      zend_object *exception)
{
    (void)event; (void)result; (void)exception;
    h3_static_try_read(((h3_static_cb_t *)callback)->state);
}

static zend_async_event_callback_t *h3_static_attach(zend_async_event_t *event,
                                                     zend_async_event_callback_fn fn,
                                                     h3_static_state_t *state)
{
    h3_static_cb_t *const cb =
        (h3_static_cb_t *)ZEND_ASYNC_EVENT_CALLBACK_EX(fn, sizeof(h3_static_cb_t));

    if (UNEXPECTED(cb == NULL)) {
        return NULL;
    }

    cb->base.dispose = h3_static_cb_dispose;
    cb->state        = state;

    if (UNEXPECTED(!event->add_callback(event, &cb->base))) {
        efree(cb);
        return NULL;
    }

    return &cb->base;
}

int h3_stream_send_static_response(void *ctx,
                                   zend_object *response_obj,
                                   zend_async_io_t *file_io,
                                   const uint64_t body_offset,
                                   const uint64_t body_length,
                                   const bool head_only_in,
                                   void (*on_done)(void *user, int status),
                                   void *user)
{
    (void)response_obj;
    http3_stream_t *s = (http3_stream_t *)ctx;

    if (UNEXPECTED(s == NULL || s->conn == NULL)) {
        return -1;
    }

    http3_connection_t *c = s->conn;

    if (UNEXPECTED(c->closed || c->nghttp3_conn == NULL)) {
        return -1;
    }

    const bool head_only = head_only_in || file_io == NULL;

    if (head_only) {
        /* HEAD with a leftover file_io: caller handed it over, dispose
         * here — no body to push. */
        if (file_io != NULL && file_io->event.dispose != NULL) {
            file_io->event.dispose(&file_io->event);
        }

        /* Buffered submit picks up :status + headers from response_obj
         * and emits an immediate 0-byte body + EOF. The data_reader
         * source is the optional response_body (set inside
         * http3_stream_submit_response when streaming==false).
         * For 304/HEAD response_body stays empty, so the EOF lands
         * on the very first reader pull. */
        if (UNEXPECTED(!http3_stream_submit_response(c, s, false))) {
            return -1;
        }

        /* Drive packets out so HEADERS leaves the wire promptly. */
        extern void http3_connection_drain_out(http3_connection_t *);
        extern void http3_connection_arm_timer(http3_connection_t *);
        http3_connection_drain_out(c);
        http3_connection_arm_timer(c);

        if (on_done != NULL) {
            on_done(user, 0);
        }

        return 0;
    }

    h3_static_budget_init_once();

    h3_static_state_t *const state = ecalloc(1, sizeof(*state));
    state->stream      = s;
    state->file_io     = file_io;
    state->body_offset = body_offset;
    state->body_length = body_length;
    state->on_done     = on_done;
    state->user        = user;

    s->tracks_static_bytes = true;
    s->static_body_state   = state;   /* teardown cancels through this */

    state->cb = h3_static_attach(&file_io->event, h3_static_read_dispatch, state);

    if (UNEXPECTED(state->cb == NULL)) {
        state->status = -1;
        h3_static_finalize(state);   /* fires on_done(-1) */
        return 0;
    }

    /* Backpressure wake. Without the event the pump would stall for good once
     * it hit the high-water mark, so treat its absence as terminal. */
    if (s->write_event == NULL) {
        s->write_event = ZEND_ASYNC_NEW_TRIGGER_EVENT();
    }

    if (UNEXPECTED(s->write_event == NULL)
        || UNEXPECTED((state->window_cb = h3_static_attach(&s->write_event->base,
                                                           h3_static_window_dispatch,
                                                           state)) == NULL)) {
        state->status = -1;
        h3_static_finalize(state);
        return 0;
    }

    h3_static_try_read(state);
    return 0;
}
