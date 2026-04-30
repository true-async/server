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
#include "php_streams.h"
#include "Zend/zend_enum.h"
#include "Zend/zend_async_API.h"

#include "log/http_log.h"
#include "core/http_connection_internal.h"   /* async_io_req_await */
#include "../../stubs/LogSeverity.php_arginfo.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#ifdef PHP_WIN32
/* winsock2.h must precede windows.h on Windows whenever a TU may end
 * up linked against networking headers from neighbouring code (project
 * convention). We don't need sockets here, but the include order is
 * cheap to honour and avoids future surprises. */
# include <winsock2.h>
# include <windows.h>
#endif

/* The state argument threaded through emit/writer is the per-server
 * log_state cached on long-lived structures (conn, mp_processor,
 * h3_connection). Multi-thread multi-server is correct by
 * construction — no globals describe "the active server".
 *
 * The writer/formatter hooks below are set once at MINIT by C
 * extensions (e.g. an OTel exporter) and are effectively read-only
 * during request processing — process-wide statics for those. */

zend_class_entry *http_log_severity_ce = NULL;

http_log_state_t  http_log_state_default = {
    .severity          = HTTP_LOG_OFF,
    .stream_set        = false,
    .async_io          = NULL,
    .writer_cb         = NULL,
    .dropped_total     = 0,
    .last_fallback_sec = 0,
};

static http_log_writer_fn    g_writer       = NULL;
static void                 *g_writer_ud    = NULL;
static http_log_formatter_fn g_formatter    = NULL;
static void                 *g_formatter_ud = NULL;

/* Drop counter + stderr-fallback rate-limit live on the per-server
 * state (see http_log_state_t in http_log.h) — drops belong to the
 * sink that lost them. */

#define HTTP_LOG_PENDING_MAX (64u * 1024u)

struct http_log_writer_cb {
    zend_async_event_callback_t  base;
    http_log_state_t            *state;
    zend_async_io_req_t         *active_req;
    char                        *active_buf;
    char                        *pending_buf;
    size_t                       pending_len;
    size_t                       pending_cap;
};

/* ------------------------------------------------------------------------ */

static uint64_t now_realtime_ns(void)
{
#ifdef PHP_WIN32
    /* FILETIME is 100ns ticks since 1601-01-01; constant subtracts the
     * delta to UNIX epoch (1970-01-01). */
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    uint64_t t = ((uint64_t)ft.dwHighDateTime << 32) | (uint64_t)ft.dwLowDateTime;
    t -= 116444736000000000ULL;
    return t * 100ULL;
#else
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
        return (uint64_t)time(NULL) * 1000000000ULL;
    }
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
#endif
}

static const char *severity_text(http_log_severity_t s)
{
    switch (s) {
        case HTTP_LOG_ERROR: return "ERROR";
        case HTTP_LOG_WARN:  return "WARN";
        case HTTP_LOG_INFO:  return "INFO";
        case HTTP_LOG_DEBUG: return "DEBUG";
        default:             return "?";
    }
}

size_t http_log_format_plain(const http_log_record_t *rec,
                             char *buf, size_t buf_len, void *ud)
{
    (void)ud;
    if (buf_len < 2) {
        return 0;
    }

    /* ISO-8601 UTC timestamp, ms precision. */
    time_t sec = (time_t)(rec->timestamp_ns / 1000000000ULL);
    uint32_t ms = (uint32_t)((rec->timestamp_ns % 1000000000ULL) / 1000000ULL);
    struct tm tm_buf;
#ifdef PHP_WIN32
    gmtime_s(&tm_buf, &sec);
#else
    gmtime_r(&sec, &tm_buf);
#endif

    char ts[40];
    snprintf(ts, sizeof ts, "%04d-%02d-%02dT%02d:%02d:%02d.%03uZ",
             tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
             tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec, ms);

    int prefix = snprintf(buf, buf_len, "%s %s %.*s",
                          ts, severity_text(rec->severity),
                          (int)rec->body_len,
                          rec->body != NULL ? rec->body : "");
    if (prefix < 0) {
        return 0;
    }
    size_t written = (size_t)prefix;
    if (written >= buf_len) {
        written = buf_len - 1;
    }

    for (size_t i = 0; i < rec->attrs_count && written < buf_len - 1; i++) {
        const http_log_attr_t *a = &rec->attrs[i];
        int n = 0;
        switch (a->type) {
            case HTTP_LOG_ATTR_STR:
                n = snprintf(buf + written, buf_len - written, " %s=%s",
                             a->key,
                             a->v.s != NULL ? a->v.s : "(null)");
                break;
            case HTTP_LOG_ATTR_I64:
                n = snprintf(buf + written, buf_len - written, " %s=%lld",
                             a->key, (long long)a->v.i64);
                break;
            case HTTP_LOG_ATTR_U64:
                n = snprintf(buf + written, buf_len - written, " %s=%llu",
                             a->key, (unsigned long long)a->v.u64);
                break;
            case HTTP_LOG_ATTR_BOOL:
                n = snprintf(buf + written, buf_len - written, " %s=%s",
                             a->key, a->v.b ? "true" : "false");
                break;
            case HTTP_LOG_ATTR_F64:
                n = snprintf(buf + written, buf_len - written, " %s=%g",
                             a->key, a->v.f64);
                break;
        }
        if (n < 0) {
            break;
        }
        written += (size_t)n;
        if (written >= buf_len) {
            written = buf_len - 1;
            break;
        }
    }

    if (written < buf_len - 1) {
        buf[written++] = '\n';
    }
    if (written < buf_len) {
        buf[written] = '\0';
    }
    return written;
}

/* Rate-limited (~1/sec per sink) stderr notice when the configured
 * sink fails. Never re-enters the logger. */
static void emit_fallback_stderr(http_log_state_t *state, const char *reason)
{
    uint64_t now_sec = now_realtime_ns() / 1000000000ULL;
    if (state == NULL || now_sec == state->last_fallback_sec) {
        return;
    }
    state->last_fallback_sec = now_sec;
    fprintf(stderr, "http_server log sink failed: %s, dropped=%llu\n",
            reason != NULL ? reason : "(unknown)",
            (unsigned long long)state->dropped_total);
}

/* Default writer keeps one ZEND_ASYNC_IO_WRITE in flight; further
 * emits coalesce into the pending buffer and are flushed by the
 * completion callback. Never suspends the calling coroutine. */

static void writer_kick_next(http_log_writer_cb_t *cb);

static void writer_complete_cb(
    zend_async_event_t          *event,
    zend_async_event_callback_t *callback,
    void                        *result,
    zend_object                 *exception)
{
    (void)event;
    (void)exception;
    http_log_writer_cb_t *cb = (http_log_writer_cb_t *)callback;
    /* We own the io exclusively, but NOTIFY broadcasts to every
     * registered callback — match by req identity defensively. */
    if (cb->active_req == NULL || result != cb->active_req) {
        return;
    }

    zend_async_io_req_t *req = cb->active_req;
    if (UNEXPECTED(req->exception != NULL)) {
        if (cb->state != NULL) {
            cb->state->dropped_total++;
        }
        emit_fallback_stderr(cb->state, "write completion exception");
        OBJ_RELEASE(req->exception);
        req->exception = NULL;
    }
    cb->active_req = NULL;
    req->dispose(req);
    if (cb->active_buf != NULL) {
        efree(cb->active_buf);
        cb->active_buf = NULL;
    }

    writer_kick_next(cb);
}

static void writer_callback_dispose(
    zend_async_event_callback_t *callback,
    zend_async_event_t          *event)
{
    (void)event;
    (void)callback;
    /* Memory is owned by the embedding state (http_log_server_stop
     * detaches and frees this struct). */
}

static void writer_kick_next(http_log_writer_cb_t *cb)
{
    /* state->async_io is NULL once the embedding state was stopped —
     * pending bytes are then unflushable; drop them silently here. */
    if (cb->state == NULL || cb->state->async_io == NULL
        || cb->pending_len == 0) {
        return;
    }
    char  *buf = cb->pending_buf;
    size_t len = cb->pending_len;

    cb->pending_buf = NULL;
    cb->pending_len = 0;
    cb->pending_cap = 0;

    cb->active_buf = buf;
    cb->active_req = ZEND_ASYNC_IO_WRITE(cb->state->async_io, buf, len);
    if (UNEXPECTED(cb->active_req == NULL)) {
        efree(buf);
        cb->active_buf = NULL;
        cb->state->dropped_total++;
        emit_fallback_stderr(cb->state, "async write submit failed");
    }
}

static void writer_append_pending(http_log_writer_cb_t *cb,
                                  const char *src, size_t len)
{
    if (cb->pending_len + len > HTTP_LOG_PENDING_MAX) {
        if (cb->state != NULL) {
            cb->state->dropped_total++;
        }
        emit_fallback_stderr(cb->state, "pending overflow");
        return;
    }
    if (cb->pending_len + len > cb->pending_cap) {
        size_t new_cap = cb->pending_cap == 0 ? 1024 : cb->pending_cap;
        while (new_cap < cb->pending_len + len) {
            new_cap *= 2;
        }
        cb->pending_buf = erealloc(cb->pending_buf, new_cap);
        cb->pending_cap = new_cap;
    }
    memcpy(cb->pending_buf + cb->pending_len, src, len);
    cb->pending_len += len;
}

static void default_writer(const http_log_record_t *rec, void *ud)
{
    (void)ud;
    http_log_state_t     *state = rec->state;
    http_log_writer_cb_t *cb    = state->writer_cb;
    if (cb == NULL || state->async_io == NULL) {
        /* The state was activated but lost its sink (mid-stop race
         * shouldn't happen single-thread, but defensive). */
        return;
    }

    char buf[2048];
    http_log_formatter_fn fmt =
        g_formatter != NULL ? g_formatter : http_log_format_plain;
    size_t n = fmt(rec, buf, sizeof buf, g_formatter_ud);
    if (n == 0) {
        return;
    }

    if (cb->active_req != NULL) {
        writer_append_pending(cb, buf, n);
        return;
    }

    /* libuv keeps the buffer pointer until completion — copy to a
     * heap slot the completion callback will free. */
    char *out = emalloc(n);
    memcpy(out, buf, n);
    cb->active_buf = out;
    cb->active_req = ZEND_ASYNC_IO_WRITE(state->async_io, out, n);
    if (UNEXPECTED(cb->active_req == NULL)) {
        efree(out);
        cb->active_buf = NULL;
        state->dropped_total++;
        emit_fallback_stderr(state, "async write submit failed");
    }
}

void http_log_set_writer(http_log_writer_fn fn, void *ud)
{
    g_writer = fn;
    g_writer_ud = ud;
}

void http_log_set_formatter(http_log_formatter_fn fn, void *ud)
{
    g_formatter = fn;
    g_formatter_ud = ud;
}

void http_log_emitf(http_log_state_t *state,
                    http_log_severity_t sev,
                    const http_log_attr_t *attrs, size_t attrs_n,
                    const char *tmpl, ...)
{
    /* Re-check the gate: callers via the macro have already gated, but
     * direct API users haven't. */
    if (state == NULL || state->severity == HTTP_LOG_OFF
        || sev == HTTP_LOG_OFF || (int)sev < (int)state->severity
        || tmpl == NULL) {
        return;
    }

    char body[1024];
    va_list ap;
    va_start(ap, tmpl);
    int n = vsnprintf(body, sizeof body, tmpl, ap);
    va_end(ap);
    if (n < 0) {
        return;
    }
    if ((size_t)n >= sizeof body) {
        n = (int)sizeof body - 1;
    }

    http_log_record_t rec = {
        .state        = state,
        .timestamp_ns = now_realtime_ns(),
        .severity     = sev,
        .tmpl         = tmpl,
        .body         = body,
        .body_len     = (size_t)n,
        .attrs        = attrs,
        .attrs_count  = attrs_n,
        .has_trace    = false,
    };

    http_log_writer_fn w = g_writer != NULL ? g_writer : default_writer;
    w(&rec, g_writer_ud);
}

void http_log_state_init(http_log_state_t *state)
{
    state->severity          = HTTP_LOG_OFF;
    state->stream_set        = false;
    state->async_io          = NULL;
    state->writer_cb         = NULL;
    state->dropped_total     = 0;
    state->last_fallback_sec = 0;
    ZVAL_UNDEF(&state->stream_zv);
}

void http_log_minit(void)
{
    g_writer        = NULL;
    g_writer_ud     = NULL;
    g_formatter     = NULL;
    g_formatter_ud  = NULL;

    http_log_severity_ce = register_class_TrueAsync_LogSeverity();
}

void http_log_mshutdown(void)
{
    /* HttpServer instances own their own state; nothing global to free. */
}

void http_log_server_start(http_log_state_t *state,
                           http_log_severity_t severity,
                           zval *stream_zv)
{
    if (state == NULL) {
        return;
    }

    /* Drain a stale sink so re-activation with a new stream doesn't leak. */
    if (state->stream_set || state->async_io != NULL || state->writer_cb != NULL) {
        http_log_server_stop(state);
    }

    if (severity == HTTP_LOG_OFF || stream_zv == NULL
        || Z_TYPE_P(stream_zv) != IS_RESOURCE) {
        state->severity = HTTP_LOG_OFF;
        return;
    }

    /* CAST_INTERNAL keeps the stream owning the fd. Non-fd streams
     * (php://memory, user wrappers) fail the cast — disable rather
     * than silently no-op. */
    php_stream *stream = NULL;
    php_stream_from_zval_no_verify(stream, stream_zv);
    if (stream == NULL) {
        state->severity = HTTP_LOG_OFF;
        return;
    }
    int fd = -1;
    int rc = php_stream_cast(stream, PHP_STREAM_AS_FD | PHP_STREAM_CAST_INTERNAL,
                             (void *)&fd, 0);
    if (rc != SUCCESS || fd < 0) {
        fprintf(stderr,
                "http_server: log stream has no underlying fd; "
                "logger disabled (use file or php://stderr)\n");
        state->severity = HTTP_LOG_OFF;
        return;
    }

    /* PRESERVE_FD: the php_stream still owns the descriptor; without
     * this flag ZEND_ASYNC_IO_CLOSE would close someone else's fd. */
    zend_async_io_t *io = ZEND_ASYNC_IO_CREATE(
        (zend_file_descriptor_t)fd,
        ZEND_ASYNC_IO_TYPE_FILE,
        ZEND_ASYNC_IO_WRITABLE | ZEND_ASYNC_IO_PRESERVE_FD);
    if (io == NULL) {
        fprintf(stderr,
                "http_server: failed to wrap log stream fd into async io; "
                "logger disabled\n");
        state->severity = HTTP_LOG_OFF;
        return;
    }

    http_log_writer_cb_t *cb = (http_log_writer_cb_t *)
        ZEND_ASYNC_EVENT_CALLBACK_EX(writer_complete_cb,
                                     sizeof(http_log_writer_cb_t));
    if (cb == NULL) {
        if (io->event.dispose != NULL) {
            io->event.dispose(&io->event);
        }
        fprintf(stderr, "http_server: failed to allocate log writer cb\n");
        state->severity = HTTP_LOG_OFF;
        return;
    }
    cb->base.dispose = writer_callback_dispose;
    cb->state        = state;
    cb->active_req   = NULL;
    cb->active_buf   = NULL;
    cb->pending_buf  = NULL;
    cb->pending_len  = 0;
    cb->pending_cap  = 0;
    if (UNEXPECTED(!io->event.add_callback(&io->event, &cb->base))) {
        efree(cb);
        if (io->event.dispose != NULL) {
            io->event.dispose(&io->event);
        }
        fprintf(stderr, "http_server: failed to attach log writer cb\n");
        state->severity = HTTP_LOG_OFF;
        return;
    }

    ZVAL_COPY(&state->stream_zv, stream_zv);
    state->stream_set = true;
    state->async_io   = io;
    state->writer_cb  = cb;
    state->severity   = severity;
}

void http_log_server_stop(http_log_state_t *state)
{
    if (state == NULL) {
        return;
    }

    state->severity = HTTP_LOG_OFF;

    /* Drain in-flight writes before tearing down — closing the io
     * with reqs still pending would UAF on completion. writer_complete_cb
     * fires from inside the await, kicks any pending coalesced bytes,
     * and we loop until idle. Requires a coroutine context, which the
     * normal HttpServer::stop() path provides. */
    if (state->async_io != NULL && state->writer_cb != NULL
        && ZEND_ASYNC_CURRENT_COROUTINE != NULL) {
        while (state->writer_cb->active_req != NULL) {
            zend_async_io_req_t *req = state->writer_cb->active_req;
            (void)async_io_req_await(req, state->async_io,
                                     /* timeout_ms */ 0, HTTP_IO_REQ_WRITE,
                                     /* log_state */ NULL);
        }
    }

    /* No coroutine to drain on — leak the cb/io/stream rather than
     * tear them down with a libuv thread-pool write still in flight. */
    if (UNEXPECTED(state->writer_cb != NULL
                   && state->writer_cb->active_req != NULL)) {
        emit_fallback_stderr(state, "teardown with in-flight write — leaking sink");
        state->writer_cb  = NULL;
        state->async_io   = NULL;
        state->stream_set = false;
        ZVAL_UNDEF(&state->stream_zv);
        return;
    }

    if (state->writer_cb != NULL) {
        http_log_writer_cb_t *cb = state->writer_cb;
        state->writer_cb = NULL;
        if (state->async_io != NULL
            && state->async_io->event.del_callback != NULL) {
            state->async_io->event.del_callback(&state->async_io->event,
                                                &cb->base);
        }
        if (cb->pending_buf != NULL) {
            efree(cb->pending_buf);
        }
        efree(cb);
    }

    if (state->async_io != NULL) {
        zend_async_io_t *io = state->async_io;
        state->async_io = NULL;
        ZEND_ASYNC_IO_CLOSE(io);
        /* Stream io types are disposed by libuv via uv_close; FILE has
         * no uv_close path so we drop our reference explicitly. */
        if (io->event.dispose != NULL) {
            io->event.dispose(&io->event);
        }
    }
    if (state->stream_set) {
        zval_ptr_dtor(&state->stream_zv);
        state->stream_set = false;
        ZVAL_UNDEF(&state->stream_zv);
    }
}
