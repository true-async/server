/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

/* HTTP/2 static-file body delivery FSM. See header for the contract.
 *
 * Lifecycle:
 *   - send_static_response builds nghttp2_nv[], submits response (with
 *     a data_provider pointing at the per-stream FSM struct on the
 *     body branch, NULL on HEAD / 304 / inline-error), drains pending
 *     control + DATA frames to the socket through the session pump,
 *     and installs an on_close hook on the stream.
 *   - data_read callback: pread() the slice from file_io->fd at
 *     body_offset + bytes_sent. Advance, set NGHTTP2_DATA_FLAG_EOF
 *     when the slice is exhausted. We never return DEFERRED — the
 *     byte source is local. nghttp2 stops asking under flow-control
 *     stalls, then resumes naturally on inbound WINDOW_UPDATE bytes
 *     (which re-enter http2_feed, which drains again).
 *   - On stream close: cb_on_stream_close fires the per-stream
 *     on_close hook, which disposes file_io and fires on_done once.
 *
 * pread vs ZEND_ASYNC_IO_READ: the data_provider runs in nghttp2
 * callback context (under nghttp2_session_mem_send), which is not a
 * coroutine and cannot suspend. pread() on a regular file fd hits
 * the page cache; the rare cold-cache stall is acceptable for
 * static-asset workloads — same trade-off nginx / lighttpd take. The
 * async OPEN/STAT in the static handler still avoids the truly
 * scary stalls (path resolution, NFS metadata).
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
#include <string.h>

#ifdef HAVE_UNISTD_H
# include <unistd.h>
#endif
#ifndef _WIN32
# include <sys/types.h>
#endif

#ifndef MSG_NOSIGNAL
# define MSG_NOSIGNAL 0
#endif

/* Submit + drain helpers exported from http2_session.c / strategy.
 * Local extern decls to keep the public headers free of these
 * internal hooks — only this TU needs them. */
extern nghttp2_session *http2_session_get_ng(http2_session_t *session);
extern void http2_static_drain_to_socket(http_connection_t *conn,
                                         http2_session_t *session);

/* Forbidden / HTTP/1-only response headers per RFC 9113 §8.2.2.
 * Mirrors the strategy's response_header_allowed but kept local. */
static bool h2_static_header_allowed(const char *name, const size_t len)
{
    if (len == 10 && strncasecmp(name, "connection",        10) == 0) return false;

    if (len == 10 && strncasecmp(name, "keep-alive",        10) == 0) return false;

    if (len == 17 && strncasecmp(name, "transfer-encoding", 17) == 0) return false;

    if (len == 7  && strncasecmp(name, "upgrade",            7) == 0) return false;

    if (len == 14 && strncasecmp(name, "content-length",    14) == 0) return false;
    return true;
}

typedef struct {
    http2_stream_t      *stream;
    zend_async_io_t     *file_io;     /* owned; disposed on finalize */
    int                  file_fd;
    uint64_t             body_offset;
    uint64_t             body_length;
    uint64_t             bytes_sent;

    void               (*on_done)(void *user, int status);
    void                *user;
    bool                 done_fired;
} h2_static_state_t;

static void h2_static_finalize(h2_static_state_t *state, int status)
{
    if (state == NULL) {
        return;
    }

    if (state->file_io != NULL) {
        if (state->file_io->event.dispose != NULL) {
            state->file_io->event.dispose(&state->file_io->event);
        }

        state->file_io = NULL;
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
static void h2_static_on_stream_close(void *user, uint32_t error_code)
{
    h2_static_state_t *state = (h2_static_state_t *)user;
    h2_static_finalize(state, error_code == NGHTTP2_NO_ERROR ? 0 : -1);
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

    if (state == NULL || state->file_fd < 0) {
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    }

    const uint64_t remaining = state->bytes_sent < state->body_length
                                   ? state->body_length - state->bytes_sent
                                   : 0;

    if (remaining == 0) {
        *data_flags |= NGHTTP2_DATA_FLAG_EOF;
        return 0;
    }

    const size_t want = remaining < length ? (size_t)remaining : length;
    const off_t pos = (off_t)(state->body_offset + state->bytes_sent);

#ifdef _WIN32
    if (lseek(state->file_fd, pos, SEEK_SET) == (off_t)-1) {
        return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
    }

    const ssize_t n = (ssize_t)read(state->file_fd, buf, want);
#else
    ssize_t n;
    do {
        n = pread(state->file_fd, buf, want, pos);
    } while (n < 0 && errno == EINTR);
#endif

    if (n <= 0) {
        return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
    }

    state->bytes_sent += (uint64_t)n;

    if (state->bytes_sent >= state->body_length) {
        *data_flags |= NGHTTP2_DATA_FLAG_EOF;
    }

    return n;
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

            if (!h2_static_header_allowed(ZSTR_VAL(name), ZSTR_LEN(name))) continue;

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

            if (!h2_static_header_allowed(ZSTR_VAL(name), ZSTR_LEN(name))) continue;

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
         * data_provider that pread()s out of file_io->fd. */
        state = ecalloc(1, sizeof(*state));
        state->stream      = stream;
        state->file_io     = file_io;
        state->file_fd     = (int)file_io->descriptor.fd;
        state->body_offset = body_offset;
        state->body_length = body_length;
        state->on_done     = on_done;
        state->user        = user;

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
