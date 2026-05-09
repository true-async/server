/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

/* HTTP/2 static-file body delivery FSM.
 *
 * Body source is read asynchronously via ZEND_ASYNC_IO_READ into a
 * per-stream 16 KiB buffer; the nghttp2 data_provider returns
 * NGHTTP2_ERR_DEFERRED while a read is in flight and resumes via
 * nghttp2_session_resume_data when the buffer is filled.
 *
 *   Lifecycle:
 *     - send_static_response builds nghttp2_nv[], submits response
 *       (with a data_provider on the body branch, NULL on HEAD / 304
 *       / inline-error), registers a persistent callback on
 *       file_io->event, drains pending control + first DATA frames to
 *       the socket, and installs an on_close hook on the stream.
 *     - data_read callback: serves bytes out of the per-stream read
 *       buffer if any are ready; otherwise submits a
 *       ZEND_ASYNC_IO_READ for the next slice and returns
 *       NGHTTP2_ERR_DEFERRED. Sets NGHTTP2_DATA_FLAG_EOF when the body
 *       slice is exhausted or the underlying file hits EOF.
 *     - dispatch callback (file_io completion): writes the bytes into
 *       the read buffer, calls nghttp2_session_resume_data, and drives
 *       http2_static_drain_to_socket so the freshly-readable DATA
 *       frame actually leaves the wire.
 *     - On stream close (cb_on_stream_close): the per-stream on_close
 *       hook fires, which detaches the persistent cb, disposes
 *       file_io, and fires on_done exactly once.
 *
 * Why async-read + DEFERRED instead of NO_COPY + sendfile(2):
 *
 *   nghttp2's `NGHTTP2_DATA_FLAG_NO_COPY` + `send_data_callback` is the
 *   API that, in principle, would let an application splice
 *   `sendfile(2)` directly into the wire instead of copying bytes
 *   through nghttp2's output buffer. In practice it is incompatible
 *   with `nghttp2_session_mem_send`-based servers (us): the callback
 *   is invoked synchronously inside `mem_send` and must produce the
 *   complete DATA frame "somewhere" — `mem_send`'s contract returns a
 *   single contiguous slice, so there is no clean splice point for a
 *   side-channel `sendfile(2)`. Real-world bugs against this combo:
 *   nghttp2 issues #796 and #817. The h2o team explicitly rejected
 *   libnghttp2 for this reason and wrote their own h2 framer
 *   (h2o/h2o#671); nginx likewise hand-rolls h2 to mix
 *   `writev()`/`sendfile(2)` chains in its socket layer
 *   (`ngx_freebsd_sendfile_chain.c`, `ngx_http_v2_filter_module.c`).
 *   nghttp2 itself recommends the data-provider read-into-buffer
 *   pattern (examples/libevent-server.c, tutorial-server.rst).
 *
 *   Consequently we do what every other nghttp2-based server does:
 *   data-provider reads into a small per-stream buffer. The only
 *   improvement we add over the textbook is that the read is
 *   asynchronous (ZEND_ASYNC_IO_READ + DEFERRED) so a cold-cache fault
 *   does not stall the event loop the way a synchronous pread() would.
 *
 *   References:
 *     - https://nghttp2.org/documentation/nghttp2_session_callbacks_set_send_data_callback.html
 *     - https://github.com/nghttp2/nghttp2/issues/796
 *     - https://github.com/nghttp2/nghttp2/issues/817
 *     - https://github.com/h2o/h2o/issues/671
 *     - https://github.com/nghttp2/nghttp2/blob/master/examples/libevent-server.c
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
#include "http_response_header_filter.h"
#include "http_response_internal.h"

#include <nghttp2/nghttp2.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

/* Per-chunk read size. 16 KiB matches HTTP2_SETTINGS_MAX_FRAME so a
 * single buffer fill always covers the largest DATA frame nghttp2 can
 * emit between flow-control updates. */
#define H2_STATIC_READ_CHUNK_BYTES (16u * 1024u)

/* Submit + drain helpers exported from http2_session.c / strategy.
 * Local extern decls to keep the public headers free of these
 * internal hooks — only this TU needs them. */
extern nghttp2_session *http2_session_get_ng(http2_session_t *session);
extern void http2_static_drain_to_socket(http_connection_t *conn,
                                         http2_session_t *session);

typedef struct {
    http2_stream_t              *stream;
    http2_session_t             *session;
    http_connection_t           *conn;

    zend_async_io_t             *file_io;     /* owned; disposed on finalize */
    uint64_t                     body_offset;
    uint64_t                     body_length;
    uint64_t                     bytes_sent;

    /* Per-stream read buffer. Single in-flight read at a time —
     * matches the pattern in http1_sendfile.c TLS chunked path. emalloc'd
     * lazily on the first DEFERRED return so HEAD/304/inline never pays
     * for it. */
    char                        *read_buf;
    size_t                       read_buf_filled;
    size_t                       read_buf_consumed;
    bool                         read_in_flight;
    bool                         eof_reached;

    /* Identity guard for spurious dispatch fires. A callback registered
     * during a NOTIFY iteration can re-enter the same iteration with a
     * stale result; matching req identity filters those out. */
    zend_async_io_req_t         *pending_req;
    zend_async_event_callback_t *cb;

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

static void h2_static_finalize(h2_static_state_t *state, const int status)
{
    if (state->cb != NULL && state->file_io != NULL) {
        (void)state->file_io->event.del_callback(&state->file_io->event,
                                                 state->cb);
        state->cb = NULL;
    }

    if (state->file_io != NULL) {
        if (state->file_io->event.dispose != NULL) {
            state->file_io->event.dispose(&state->file_io->event);
        }

        state->file_io = NULL;
    }

    if (state->read_buf != NULL) {
        efree(state->read_buf);
        state->read_buf = NULL;
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
    h2_static_finalize(state, error_code == NGHTTP2_NO_ERROR ? 0 : -1);
}

/* file_io completion dispatch. Identity-guarded against stale fires.
 * On bytes: parks them in read_buf, wakes nghttp2's data_provider, and
 * drives one drain so the freshly-available DATA frame leaves the
 * wire. On error / EOF before slice end: marks state and lets the
 * data_provider finalize through nghttp2_session_resume_data. */
static void h2_static_dispatch(zend_async_event_t *event,
                               zend_async_event_callback_t *callback,
                               void *result,
                               zend_object *exception)
{
    (void)event;
    h2_static_state_t *state = ((h2_static_cb_t *)callback)->state;
    zend_async_io_req_t *req = (zend_async_io_req_t *)result;

    if (req == NULL || req != state->pending_req) {
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

    if (UNEXPECTED(err) || transferred < 0) {
        state->status = -1;
        state->eof_reached = true;
    } else if (transferred == 0) {
        state->eof_reached = true;
    } else {
        state->read_buf_filled = (size_t)transferred;
        state->read_buf_consumed = 0;
    }

    if (state->session == NULL) {
        return;
    }

    nghttp2_session *ng = http2_session_get_ng(state->session);

    if (ng == NULL) {
        return;
    }

    if (state->stream != NULL && !state->stream->peer_closed) {
        (void)nghttp2_session_resume_data(ng,
            (int32_t)state->stream->stream_id);

        if (state->conn != NULL) {
            http2_static_drain_to_socket(state->conn, state->session);
        }
    }
}

/* Submit the next ZEND_ASYNC_IO_READ. Lazy-allocates read_buf on
 * first call. Sets read_in_flight on success. Returns false on submit
 * failure — caller forwards as NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE. */
static bool h2_static_submit_read(h2_static_state_t *state)
{
    if (state->file_io == NULL) {
        return false;
    }

    if (state->read_buf == NULL) {
        state->read_buf = emalloc(H2_STATIC_READ_CHUNK_BYTES);
    }

    const uint64_t remaining = state->body_length > state->bytes_sent
                                   ? state->body_length - state->bytes_sent
                                   : 0;

    if (remaining == 0) {
        state->eof_reached = true;
        return true;
    }

    const size_t want = remaining < H2_STATIC_READ_CHUNK_BYTES
                            ? (size_t)remaining
                            : H2_STATIC_READ_CHUNK_BYTES;

    state->pending_req = ZEND_ASYNC_IO_READ(state->file_io,
                                            state->read_buf, want);

    if (state->pending_req == NULL) {
        return false;
    }

    state->read_in_flight = true;
    return true;
}

static ssize_t h2_static_data_read(nghttp2_session *ng,
                                   const int32_t stream_id,
                                   uint8_t *buf,
                                   const size_t length,
                                   uint32_t *data_flags,
                                   nghttp2_data_source *source,
                                   void *user_data)
{
    (void)ng;
    (void)stream_id;
    (void)user_data;

    h2_static_state_t *state = (h2_static_state_t *)source->ptr;

    if (state == NULL) {
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    }

    if (UNEXPECTED(state->status != 0)) {
        return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
    }

    /* Serve out of the read buffer if any bytes are ready. */
    const size_t avail =
        state->read_buf_filled > state->read_buf_consumed
            ? state->read_buf_filled - state->read_buf_consumed
            : 0;

    if (avail > 0) {
        const size_t to_copy = avail < length ? avail : length;
        memcpy(buf,
               state->read_buf + state->read_buf_consumed,
               to_copy);
        state->read_buf_consumed += to_copy;
        state->bytes_sent += (uint64_t)to_copy;

        const bool buf_drained =
            state->read_buf_consumed >= state->read_buf_filled;

        if (buf_drained) {
            state->read_buf_filled = 0;
            state->read_buf_consumed = 0;
        }

        if (state->bytes_sent >= state->body_length) {
            *data_flags |= NGHTTP2_DATA_FLAG_EOF;
            return (ssize_t)to_copy;
        }

        /* If the buffer is now drained and there's still body left,
         * pre-arm the next read so the next data_provider invocation
         * can return DEFERRED immediately rather than re-entering with
         * stale buffer pointers. */
        if (buf_drained && !state->read_in_flight) {
            if (UNEXPECTED(!h2_static_submit_read(state))) {
                state->status = -1;
                /* Bytes already returned this call are valid; the
                 * subsequent invocation will see status != 0 and bail. */
            }
        }

        return (ssize_t)to_copy;
    }

    /* Buffer empty. Either EOF, in-flight, or we need to submit. */
    if (state->eof_reached || state->bytes_sent >= state->body_length) {
        *data_flags |= NGHTTP2_DATA_FLAG_EOF;
        return 0;
    }

    if (state->read_in_flight) {
        return NGHTTP2_ERR_DEFERRED;
    }

    if (UNEXPECTED(!h2_static_submit_read(state))) {
        return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
    }

    if (state->eof_reached) {
        *data_flags |= NGHTTP2_DATA_FLAG_EOF;
        return 0;
    }

    return NGHTTP2_ERR_DEFERRED;
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
            /* Pin the inline body on the stream so the existing
             * buffered data_provider in http2_session.c can serve it.
             * Response zval lifetime brackets the stream — pointer
             * stable for the duration of the drain. */
            stream->response_body        = ZSTR_VAL(inline_body);
            stream->response_body_len    = ZSTR_LEN(inline_body);
            stream->response_body_offset = 0;

            extern ssize_t /* http2_response_data_read */
                http2_static_buffered_data_read(nghttp2_session *,
                                                int32_t, uint8_t *, size_t,
                                                uint32_t *,
                                                nghttp2_data_source *,
                                                void *);
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
        /* Body branch: allocate FSM state, install close hook, register
         * persistent dispatch cb on file_io, register the async-read
         * data_provider. */
        state = ecalloc(1, sizeof(*state));
        state->stream      = stream;
        state->session     = stream->session;
        state->conn        = conn;
        state->file_io     = file_io;
        state->body_offset = body_offset;
        state->body_length = body_length;
        state->on_done     = on_done;
        state->user        = user;

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

        stream->on_close      = h2_static_on_stream_close;
        stream->on_close_user = state;

        nghttp2_data_provider prv;
        prv.source.ptr   = state;
        prv.read_callback = h2_static_data_read;
        rc = nghttp2_submit_response(ng, (int32_t)stream->stream_id,
                                     nv, nv_count, &prv);
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
    http2_static_drain_to_socket(conn, stream->session);

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
