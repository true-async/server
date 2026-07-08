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
 * Transport: a per-request producer coroutine reads the file in 16 KiB
 * chunks via ZEND_ASYNC_IO_READ, wraps each into a zend_string, and
 * pushes through h3_stream_append_chunk. That hooks the existing
 * streaming machinery on the H/3 side — chunk_queue + nghttp3
 * data_reader + acked_stream_data release + window-extend
 * backpressure. EOF triggers h3_stream_mark_ended; the data_reader
 * flags FIN on the next pull.
 *
 * Why a coroutine and not a callback FSM (as in http1_sendfile.c):
 *   h3_stream_append_chunk already implements the canonical
 *   backpressure suspend on the stream's write_event (peer
 *   MAX_STREAM_DATA), and that suspend is only valid from coroutine
 *   context. Driving the pump from inside a libuv read-callback would
 *   either need a parallel non-suspending fast path (duplicates the
 *   queue-management logic) or ignore backpressure entirely
 *   (unbounded chunk_queue growth on slow peers). A dedicated producer
 *   coroutine reuses the streaming path verbatim — same code path
 *   that HttpResponse::send() exercises.
 *
 * Why no NO_COPY/sendfile fast path on H/3:
 *   QUIC always encrypts. Kernel sendfile(2) cannot splice plaintext
 *   into an encrypted stream; only kTLS+sendfile is the known kernel
 *   path, and it doesn't extend to QUIC. nghttp3's data_reader API
 *   has no zero-copy hook either. Same trade-off applies on H/2 (see
 *   src/http2/http2_static_response.c top-of-file comment for the
 *   nghttp2 NO_COPY discussion + references).
 *
 * Lifecycle:
 *   - send_static_response called from engine_delegate_to_protocol.
 *     HEAD/304/empty-body branch: submit response with NULL body,
 *     fire on_done(0), return.
 *   - Body branch: spawn a producer coroutine, transfer ownership of
 *     file_io to it. Headers commit lazily on the first
 *     append_chunk (same as the user-driven streaming path).
 *   - Producer loop: ZEND_ASYNC_IO_READ → zend_string → append_chunk
 *     until body_length sent or EOF. Then mark_ended + on_done.
 *   - Cancellation: if the peer resets the stream, append_chunk
 *     short-circuits with HTTP_STREAM_APPEND_STREAM_DEAD; loop exits,
 *     finalize fires on_done(-1).
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

/* Per-worker static-delivery memory cap across concurrent streams.
 * Formula and clamps mirror http2_static_response.c. */
#define H3_STATIC_BUDGET_FLOOR      (4u * 1024u * 1024u)
#define H3_STATIC_BUDGET_FALLBACK   (64u * 1024u * 1024u)  /* memory_limit = -1 */
#define H3_STATIC_BUDGET_RESERVE_FRAC 8u                   /* hard top = 87.5% */
#define H3_STATIC_THROTTLE_POLL_MS  2u                     /* pump re-check cadence */

/* 0 = not yet initialised (h3_static_budget_init_once). */
ZEND_TLS size_t h3_static_budget_bytes           = 0;
ZEND_TLS size_t h3_static_global_bytes_in_flight = 0;
ZEND_TLS size_t h3_static_global_high_water      = 0;

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

void h3_static_account_debit(const size_t n)
{
    /* Clamp: the one-shot teardown remainder can overlap chunks already
     * debited on the ACK path if a release raced, so never underflow. */
    h3_static_global_bytes_in_flight -=
        (n <= h3_static_global_bytes_in_flight) ? n : h3_static_global_bytes_in_flight;
}

static bool h3_static_over_budget(void)
{
    return h3_static_global_bytes_in_flight >= h3_static_budget_bytes;
}

/* Suspend the pump on a one-shot timer so the reactor keeps draining/ACKing. */
static void h3_static_throttle_sleep(zend_coroutine_t *co, const zend_ulong ms)
{
    zend_async_timer_event_t *const t = ZEND_ASYNC_NEW_TIMER_EVENT(ms, false);

    if (UNEXPECTED(t == NULL)) {
        zend_clear_exception();
        return;
    }

    t->base.start(&t->base);
    zend_async_resume_when(co, &t->base, true, zend_async_waker_callback_resolve, NULL);
    ZEND_ASYNC_SUSPEND();
    ZEND_ASYNC_WAKER_DESTROY(co);
}

typedef struct {
    http3_stream_t              *stream;
    http3_connection_t          *conn;

    zend_async_io_t             *file_io;
    uint64_t                     body_offset;
    uint64_t                     body_length;
    uint64_t                     bytes_sent;

    void                       (*on_done)(void *user, int status);
    void                        *user;
    int                          status;
    bool                         done_fired;
} h3_static_state_t;

static void h3_static_finalize(h3_static_state_t *state)
{
    if (state->file_io != NULL) {
        if (state->file_io->event.dispose != NULL) {
            state->file_io->event.dispose(&state->file_io->event);
        }

        state->file_io = NULL;
    }

    void (*on_done)(void *, int) = state->on_done;
    void *user = state->user;
    const int status = state->status;
    const bool fire = !state->done_fired && on_done != NULL;
    state->done_fired = true;

    efree(state);

    if (fire) {
        on_done(user, status);
    }
}

/* Producer coroutine entry. Reads the file slice in 16 KiB chunks and
 * pushes each through h3_stream_append_chunk. Blocks naturally on
 * backpressure inside append_chunk; bails on stream death. */
static void h3_static_pump_entry(void)
{
    zend_coroutine_t *co = ZEND_ASYNC_CURRENT_COROUTINE;
    h3_static_state_t *state = (h3_static_state_t *)co->extended_data;

    if (state == NULL || state->stream == NULL || state->conn == NULL) {
        return;
    }

    h3_static_budget_init_once();
    state->stream->tracks_static_bytes = true;

    char *buf = emalloc(H3_STATIC_READ_CHUNK_BYTES);

    while (state->bytes_sent < state->body_length
           && !state->stream->peer_closed) {
        /* over budget: pace this stream; queued bytes keep draining, so no deadlock */
        while (h3_static_over_budget() && !state->stream->peer_closed) {
            h3_static_throttle_sleep(co, H3_STATIC_THROTTLE_POLL_MS);

            if (EG(exception) != NULL) {
                zend_clear_exception();
                break;
            }
        }

        const uint64_t remaining = state->body_length - state->bytes_sent;
        const size_t want = remaining < H3_STATIC_READ_CHUNK_BYTES
                                ? (size_t)remaining
                                : H3_STATIC_READ_CHUNK_BYTES;

        zend_async_io_req_t *req = ZEND_ASYNC_IO_READ(state->file_io,
                                                      buf, want);

        if (UNEXPECTED(req == NULL)) {
            /* Submit failed: the reactor left an exception in EG. This pump runs
             * in a coroutine, so absorb it the way the socket-write submit path
             * does — otherwise it surfaces as an uncaught top-level fatal when
             * the coroutine unwinds. status=-1 lets the on_done policy retire the
             * stream. (The completion-error case below is already absorbed via
             * req->exception.) */
            if (EG(exception) != NULL) {
                zend_clear_exception();
            }

            state->status = -1;
            break;
        }

        const bool ok = async_io_req_await(req, state->file_io,
                                           /* timeout_ms */ 0,
                                           HTTP_IO_REQ_READ,
                                           /* log_state */ NULL);
        const ssize_t got = req->transferred;
        const bool err = (req->exception != NULL);

        if (req->exception != NULL) {
            OBJ_RELEASE(req->exception);
            req->exception = NULL;
        }

        if (req->dispose != NULL) {
            req->dispose(req);
        }

        if (UNEXPECTED(!ok || err || got < 0)) {
            state->status = -1;
            break;
        }

        if (got == 0) {
            /* EOF before slice end — file truncated under us. The
             * bytes already on the wire belong to the peer; let
             * keep-alive policy decide. */
            state->status = -1;
            break;
        }

        /* Hand the chunk to the streaming path. append_chunk takes
         * ownership of the zend_string ref. The string's backing
         * memory mirrors the freshly-read 16 KiB slice. */
        zend_string *chunk = zend_string_init(buf, (size_t)got, 0);
        const int rc = h3_stream_append_chunk(state->stream, chunk);

        if (rc != HTTP_STREAM_APPEND_OK) {
            /* append_chunk released the chunk on the dead-stream branch. */
            state->status = -1;
            break;
        }

        state->bytes_sent += (uint64_t)got;
    }

    efree(buf);

    /* Always call mark_ended — it short-circuits if peer_closed and
     * is otherwise needed to flag EOF on the data_reader. The drain
     * out happens inside mark_ended (and earlier inside append_chunk). */
    if (state->status == 0) {
        h3_stream_mark_ended(state->stream);
    }

    h3_static_finalize(state);
}

/* Coroutine dispose. Fires when the coroutine returns or is cancelled
 * (e.g. server shutdown). state has already been freed by the pump
 * entry on the normal-exit path; on cancel-without-entry we still own
 * it, so finalize defensively. */
static void h3_static_pump_dispose(zend_coroutine_t *co)
{
    h3_static_state_t *state = (h3_static_state_t *)co->extended_data;

    if (state == NULL || state->done_fired) {
        return;
    }

    state->status = -1;
    h3_static_finalize(state);
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

    http_server_object *server =
        (http_server_object *)http3_listener_server_obj(c->listener);

    if (UNEXPECTED(server == NULL)) {
        return -1;
    }

    zend_async_scope_t *scope = http_server_get_scope(server);

    if (UNEXPECTED(scope == NULL)) {
        return -1;
    }

    h3_static_state_t *state = ecalloc(1, sizeof(*state));
    state->stream      = s;
    state->conn        = c;
    state->file_io     = file_io;
    state->body_offset = body_offset;
    state->body_length = body_length;
    state->on_done     = on_done;
    state->user        = user;

    zend_coroutine_t *co = ZEND_ASYNC_NEW_COROUTINE(scope);

    if (UNEXPECTED(co == NULL)) {
        h3_static_finalize(state);  /* fires on_done(-1) */
        state = NULL;
        return -1;
    }

    co->internal_entry   = h3_static_pump_entry;
    co->extended_data    = state;
    co->extended_dispose = h3_static_pump_dispose;

    ZEND_ASYNC_ENQUEUE_COROUTINE(co);
    (void)response_obj;
    return 0;
}
