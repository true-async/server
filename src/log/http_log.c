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
#include "Zend/zend_exceptions.h"   /* zend_clear_exception */
#include "Zend/zend_async_API.h"

#include "log/http_log.h"
#include "core/async_plain_event.h"   /* drain-flush wakeup */
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

/* The state argument threaded through emit is the per-server log_state
 * cached on long-lived structures (conn, mp_processor, h3_connection).
 * Multi-thread multi-server is correct by construction — no globals
 * describe "the active server", and each sink owns its own transport. */

zend_class_entry *http_log_severity_ce = NULL;

http_log_state_t  http_log_state_default = {
    .severity   = HTTP_LOG_OFF,
    .sink_count = 0,
};

#define HTTP_LOG_PENDING_MAX (64u * 1024u)

/* Ceiling on the total stop-time flush wait across all sinks; a wedged sink
 * past this leaks rather than pinning teardown. */
#define HTTP_LOG_STOP_DRAIN_BUDGET_MS 3000u

/* Distinct formatters cached per emit before fan-out. The built-in set is
 * plain/json/logfmt/pretty, so a real config never overflows; this also
 * bounds the emit-path stack to ~8 KiB of format buffers. */
#define HTTP_LOG_FMT_SLOTS 4

struct http_log_writer_cb {
    zend_async_event_callback_t  base;
    http_log_sink_t             *sink;
    zend_async_io_req_t         *active_req;
    char                        *active_buf;
    char                        *pending_buf;
    size_t                       pending_len;
    size_t                       pending_cap;
    /* Fired by writer_complete_cb once the write chain fully drains, so
     * http_log_server_stop can wait for flush without polling. Same thread
     * as the completion, so a plain (non-uv) event suffices. */
    zend_async_event_t          *drain_event;
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

/* Rate-limited (~1/sec per sink) stderr notice when a sink's transport
 * fails. Never re-enters the logger. */
static void emit_fallback_stderr(http_log_sink_t *sink, const char *reason)
{
    uint64_t now_sec = now_realtime_ns() / 1000000000ULL;

    if (sink == NULL || now_sec == sink->last_fallback_sec) {
        return;
    }

    sink->last_fallback_sec = now_sec;
    fprintf(stderr, "http_server log sink failed: %s, dropped=%llu\n",
            reason != NULL ? reason : "(unknown)",
            (unsigned long long)sink->dropped_total);
}

/* Async file-writer transport. Keeps one ZEND_ASYNC_IO_WRITE in flight;
 * further emits coalesce into the pending buffer and are flushed by the
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
        if (cb->sink != NULL) {
            cb->sink->dropped_total++;
        }

        emit_fallback_stderr(cb->sink, "write completion exception");
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

    /* Chain fully drained — wake a stop() waiting to flush before teardown. */
    if (cb->drain_event != NULL && cb->active_req == NULL) {
        async_plain_event_fire(cb->drain_event);
    }
}

static void writer_callback_dispose(
    zend_async_event_callback_t *callback,
    zend_async_event_t          *event)
{
    (void)event;
    (void)callback;
    /* Memory is owned by the embedding sink (http_log_sink_stop detaches
     * and frees this struct). */
}

static void writer_kick_next(http_log_writer_cb_t *cb)
{
    /* sink->async_io is NULL once the sink was stopped — pending bytes are
     * then unflushable; drop them silently here. */
    if (cb->sink == NULL || cb->sink->async_io == NULL
        || cb->pending_len == 0) {
        return;
    }

    char  *buf = cb->pending_buf;
    size_t len = cb->pending_len;

    cb->pending_buf = NULL;
    cb->pending_len = 0;
    cb->pending_cap = 0;

    cb->active_buf = buf;
    cb->active_req = ZEND_ASYNC_IO_WRITE(cb->sink->async_io, buf, len);

    if (UNEXPECTED(cb->active_req == NULL)) {
        efree(buf);
        cb->active_buf = NULL;
        cb->sink->dropped_total++;
        emit_fallback_stderr(cb->sink, "async write submit failed");
    }
}

static void writer_append_pending(http_log_writer_cb_t *cb,
                                  const char *src, size_t len)
{
    if (cb->pending_len + len > HTTP_LOG_PENDING_MAX) {
        if (cb->sink != NULL) {
            cb->sink->dropped_total++;
        }

        emit_fallback_stderr(cb->sink, "pending overflow");
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

/* Hand one already-formatted record to a sink's transport. */
static void http_log_sink_write(http_log_sink_t *sink, const char *buf, size_t len)
{
    http_log_writer_cb_t *cb = sink->writer_cb;

    if (cb == NULL || sink->async_io == NULL) {
        return;
    }

    if (cb->active_req != NULL) {
        writer_append_pending(cb, buf, len);
        return;
    }

    /* libuv keeps the buffer pointer until completion — copy to a
     * heap slot the completion callback will free. */
    char *out = emalloc(len);
    memcpy(out, buf, len);
    cb->active_buf = out;
    cb->active_req = ZEND_ASYNC_IO_WRITE(sink->async_io, out, len);

    if (UNEXPECTED(cb->active_req == NULL)) {
        efree(out);
        cb->active_buf = NULL;
        sink->dropped_total++;
        emit_fallback_stderr(sink, "async write submit failed");
    }
}

void http_log_emitf(http_log_state_t *state,
                    http_log_severity_t sev,
                    const http_log_attr_t *attrs, size_t attrs_n,
                    const char *tmpl, ...)
{
    /* Re-check the gate: callers via the macro have already gated, but
     * direct API users haven't. state->severity is the min sink floor. */
    if (state == NULL || state->severity == HTTP_LOG_OFF
        || sev == HTTP_LOG_OFF || (int)sev < (int)state->severity
        || tmpl == NULL || state->sink_count == 0) {
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

    /* Format once per distinct (formatter, ud), then fan out to every sink
     * whose floor admits sev. A cache miss past HTTP_LOG_FMT_SLOTS just
     * reformats into the last slot — correct, only un-deduped. */
    struct { http_log_formatter_fn fn; void *ud; size_t len; } meta[HTTP_LOG_FMT_SLOTS];
    char fbuf[HTTP_LOG_FMT_SLOTS][2048];
    int  slots = 0;

    for (uint8_t i = 0; i < state->sink_count; i++) {
        http_log_sink_t *sink = &state->sinks[i];

        if (sink->severity_floor == HTTP_LOG_OFF
            || (int)sev < (int)sink->severity_floor) {
            continue;
        }

        int s = -1;
        for (int k = 0; k < slots; k++) {
            if (meta[k].fn == sink->formatter && meta[k].ud == sink->formatter_ud) {
                s = k;
                break;
            }
        }

        char  *buf;
        size_t len;

        if (s >= 0) {
            buf = fbuf[s];
            len = meta[s].len;
        } else if (slots < HTTP_LOG_FMT_SLOTS) {
            s = slots++;
            meta[s].fn  = sink->formatter;
            meta[s].ud  = sink->formatter_ud;
            meta[s].len = sink->formatter(&rec, fbuf[s], sizeof fbuf[s],
                                          sink->formatter_ud);
            buf = fbuf[s];
            len = meta[s].len;
        } else {
            buf = fbuf[HTTP_LOG_FMT_SLOTS - 1];
            len = sink->formatter(&rec, buf, sizeof fbuf[0], sink->formatter_ud);
        }

        if (len == 0) {
            continue;
        }

        http_log_sink_write(sink, buf, len);
    }
}

void http_log_state_init(http_log_state_t *state)
{
    memset(state, 0, sizeof *state);
    state->severity   = HTTP_LOG_OFF;
    state->sink_count = 0;
}

void http_log_minit(void)
{
    http_log_severity_ce = register_class_TrueAsync_LogSeverity();
}

void http_log_mshutdown(void)
{
    /* HttpServer instances own their own state; nothing global to free. */
}

/* Recompute the fast gate: the lowest floor across all live sinks, so a
 * severity below every sink short-circuits in the macro's single branch. */
static void http_log_state_refresh_gate(http_log_state_t *state)
{
    http_log_severity_t floor = HTTP_LOG_OFF;

    for (uint8_t i = 0; i < state->sink_count; i++) {
        http_log_severity_t f = state->sinks[i].severity_floor;

        if (f == HTTP_LOG_OFF) {
            continue;
        }

        if (floor == HTTP_LOG_OFF || (int)f < (int)floor) {
            floor = f;
        }
    }

    state->severity = floor;
}

/* Build one sink onto a zeroed slot: cast the stream to an fd, wrap it in
 * async io, and attach the coalescing writer. Returns false (sink left
 * inactive, floor OFF) on any failure. */
static bool http_log_sink_start(http_log_sink_t *sink,
                                http_log_severity_t severity,
                                http_log_formatter_fn formatter,
                                void *formatter_ud,
                                zval *stream_zv)
{
    memset(sink, 0, sizeof *sink);
    sink->severity_floor = HTTP_LOG_OFF;
    ZVAL_UNDEF(&sink->stream_zv);

    if (severity == HTTP_LOG_OFF || stream_zv == NULL
        || Z_TYPE_P(stream_zv) != IS_RESOURCE) {
        return false;
    }

    /* CAST_INTERNAL keeps the stream owning the fd. Non-fd streams
     * (php://memory, user wrappers) fail the cast — disable rather
     * than silently no-op. */
    php_stream *stream = NULL;
    php_stream_from_zval_no_verify(stream, stream_zv);

    if (stream == NULL) {
        return false;
    }

    int fd = -1;
    int rc = php_stream_cast(stream, PHP_STREAM_AS_FD | PHP_STREAM_CAST_INTERNAL,
                             (void *)&fd, 0);

    if (rc != SUCCESS || fd < 0) {
        fprintf(stderr,
                "http_server: log stream has no underlying fd; "
                "sink disabled (use file or php://stderr)\n");
        return false;
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
                "sink disabled\n");
        return false;
    }

    http_log_writer_cb_t *cb = (http_log_writer_cb_t *)
        ZEND_ASYNC_EVENT_CALLBACK_EX(writer_complete_cb,
                                     sizeof(http_log_writer_cb_t));

    if (cb == NULL) {
        if (io->event.dispose != NULL) {
            io->event.dispose(&io->event);
        }

        fprintf(stderr, "http_server: failed to allocate log writer cb\n");
        return false;
    }

    cb->base.dispose = writer_callback_dispose;
    cb->sink         = sink;
    cb->active_req   = NULL;
    cb->active_buf   = NULL;
    cb->pending_buf  = NULL;
    cb->pending_len  = 0;
    cb->pending_cap  = 0;
    cb->drain_event  = NULL;

    if (UNEXPECTED(!io->event.add_callback(&io->event, &cb->base))) {
        efree(cb);

        if (io->event.dispose != NULL) {
            io->event.dispose(&io->event);
        }

        fprintf(stderr, "http_server: failed to attach log writer cb\n");
        return false;
    }

    ZVAL_COPY(&sink->stream_zv, stream_zv);
    sink->stream_set     = true;
    sink->async_io       = io;
    sink->writer_cb      = cb;
    sink->formatter      = formatter != NULL ? formatter : http_log_format_plain;
    sink->formatter_ud   = formatter_ud;
    sink->severity_floor = severity;
    return true;
}

/* Wait up to budget_ms for a sink's in-flight write chain to drain. Waits on
 * a plain event fired by writer_complete_cb (same thread) rather than the io
 * req directly — the completion frees the req, so awaiting it is a UAF. A
 * timer caps the wait so a wedged write falls to the leak path in stop. */
static void http_log_sink_drain(http_log_sink_t *sink, uint32_t budget_ms)
{
    if (budget_ms == 0 || sink->async_io == NULL || sink->writer_cb == NULL
        || sink->writer_cb->active_req == NULL
        || ZEND_ASYNC_CURRENT_COROUTINE == NULL) {
        return;
    }

    zend_coroutine_t *const co = ZEND_ASYNC_CURRENT_COROUTINE;
    http_log_writer_cb_t *const cb = sink->writer_cb;

    if (cb->drain_event == NULL) {
        cb->drain_event = async_plain_event_new();
    }

    /* Same thread: no completion can fire between this check and SUSPEND,
     * so no lost wakeup. drain_event only fires once the chain is empty. */
    if (cb->drain_event != NULL && ZEND_ASYNC_WAKER_NEW(co) != NULL) {
        zend_async_resume_when(co, cb->drain_event, false,
                               zend_async_waker_callback_resolve, NULL);
        zend_async_event_t *const timer =
            &ZEND_ASYNC_NEW_TIMER_EVENT((zend_ulong)budget_ms, false)->base;
        zend_async_resume_when(co, timer, true,
                               zend_async_waker_callback_timeout, NULL);
        ZEND_ASYNC_SUSPEND();
        zend_async_waker_clean(co);

        if (EG(exception)) {
            zend_clear_exception();   /* budget elapsed → leak path below */
        }
    }
}

/* Tear a single sink down: free the writer cb, close the io, drop the stream
 * ref. Assumes the caller already drained (or budgeted the drain away). */
static void http_log_sink_stop(http_log_sink_t *sink)
{
    /* No coroutine drained the write — leak the cb/io/stream rather than
     * tear them down with a libuv thread-pool write still in flight. */
    if (UNEXPECTED(sink->writer_cb != NULL
                   && sink->writer_cb->active_req != NULL)) {
        emit_fallback_stderr(sink, "teardown with in-flight write — leaking sink");
        sink->writer_cb      = NULL;
        sink->async_io       = NULL;
        sink->stream_set     = false;
        sink->severity_floor = HTTP_LOG_OFF;
        ZVAL_UNDEF(&sink->stream_zv);
        return;
    }

    if (sink->writer_cb != NULL) {
        http_log_writer_cb_t *cb = sink->writer_cb;
        sink->writer_cb = NULL;

        if (sink->async_io != NULL
            && sink->async_io->event.del_callback != NULL) {
            sink->async_io->event.del_callback(&sink->async_io->event,
                                               &cb->base);
        }

        if (cb->pending_buf != NULL) {
            efree(cb->pending_buf);
        }

        if (cb->drain_event != NULL) {
            cb->drain_event->dispose(cb->drain_event);
        }

        efree(cb);
    }

    if (sink->async_io != NULL) {
        zend_async_io_t *io = sink->async_io;
        sink->async_io = NULL;
        ZEND_ASYNC_IO_CLOSE(io);
        /* Stream io types are disposed by libuv via uv_close; FILE has
         * no uv_close path so we drop our reference explicitly. */
        if (io->event.dispose != NULL) {
            io->event.dispose(&io->event);
        }
    }

    if (sink->stream_set) {
        zval_ptr_dtor(&sink->stream_zv);
        sink->stream_set = false;
        ZVAL_UNDEF(&sink->stream_zv);
    }

    sink->severity_floor = HTTP_LOG_OFF;
}

void http_log_server_start(http_log_state_t *state,
                           http_log_severity_t severity,
                           zval *stream_zv)
{
    if (state == NULL) {
        return;
    }

    /* Drain a stale config so re-activation with a new stream doesn't leak. */
    if (state->sink_count > 0) {
        http_log_server_stop(state);
    }

    state->sink_count = 0;
    state->severity   = HTTP_LOG_OFF;

    /* B1: exactly one sink (the plain-formatted stream). setLogSinks (B4)
     * fills the rest of the array through this same http_log_sink_start. */
    if (http_log_sink_start(&state->sinks[0], severity,
                            http_log_format_plain, NULL, stream_zv)) {
        state->sink_count = 1;
        http_log_state_refresh_gate(state);
    }
}

void http_log_server_stop(http_log_state_t *state)
{
    if (state == NULL) {
        return;
    }

    state->severity = HTTP_LOG_OFF;   /* stop new emits immediately */

    /* One shared budget bounds the total drain wait across sinks: each waits
     * on its own drain_event, and an absolute deadline keeps the sum capped
     * even when several sinks have writes in flight. */
    uint64_t start_ns = now_realtime_ns();

    for (uint8_t i = 0; i < state->sink_count; i++) {
        uint64_t elapsed_ms = (now_realtime_ns() - start_ns) / 1000000ULL;
        uint32_t budget = elapsed_ms >= HTTP_LOG_STOP_DRAIN_BUDGET_MS
                        ? 0u
                        : (uint32_t)(HTTP_LOG_STOP_DRAIN_BUDGET_MS - elapsed_ms);

        http_log_sink_drain(&state->sinks[i], budget);
        http_log_sink_stop(&state->sinks[i]);
    }

    state->sink_count = 0;
}
