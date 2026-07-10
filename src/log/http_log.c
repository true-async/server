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
# include <io.h>       /* _isatty */
#else
# include <unistd.h>   /* isatty */
#endif

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

/* Bounded string builder over the formatter's fixed output buffer. Every
 * append is capped at cap-1 and keeps the buffer NUL-terminated, so a record
 * that overruns the buffer truncates instead of writing past it. `len` is the
 * byte count the formatter returns (excludes the NUL). */
typedef struct {
    char   *buf;
    size_t  cap;
    size_t  len;
} log_sbuf_t;

static void sb_init(log_sbuf_t *sb, char *buf, size_t cap)
{
    sb->buf = buf;
    sb->cap = cap;
    sb->len = 0;
    if (cap > 0) {
        buf[0] = '\0';
    }
}

static void sb_write(log_sbuf_t *sb, const char *s, size_t n)
{
    for (size_t i = 0; i < n && sb->len + 1 < sb->cap; i++) {
        sb->buf[sb->len++] = s[i];
    }
    if (sb->len < sb->cap) {
        sb->buf[sb->len] = '\0';
    }
}

static void sb_putc(log_sbuf_t *sb, char c)
{
    sb_write(sb, &c, 1);
}

static void sb_puts(log_sbuf_t *sb, const char *s)
{
    sb_write(sb, s, strlen(s));
}

static void sb_printf(log_sbuf_t *sb, const char *fmt, ...)
{
    if (sb->len + 1 >= sb->cap) {
        return;
    }

    size_t avail = sb->cap - sb->len;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(sb->buf + sb->len, avail, fmt, ap);
    va_end(ap);

    if (n < 0) {
        return;
    }

    sb->len += ((size_t)n >= avail) ? avail - 1 : (size_t)n;
}

static void sb_put_hex(log_sbuf_t *sb, const uint8_t *bytes, size_t n)
{
    static const char hexd[] = "0123456789abcdef";

    for (size_t i = 0; i < n; i++) {
        sb_putc(sb, hexd[bytes[i] >> 4]);
        sb_putc(sb, hexd[bytes[i] & 0x0f]);
    }
}

/* JSON string per RFC 8259: quote and escape the mandatory controls. */
static void sb_put_json_str(log_sbuf_t *sb, const char *s, size_t n)
{
    sb_putc(sb, '"');

    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];

        switch (c) {
            case '"':  sb_puts(sb, "\\\""); break;
            case '\\': sb_puts(sb, "\\\\"); break;
            case '\n': sb_puts(sb, "\\n");  break;
            case '\r': sb_puts(sb, "\\r");  break;
            case '\t': sb_puts(sb, "\\t");  break;
            case '\b': sb_puts(sb, "\\b");  break;
            case '\f': sb_puts(sb, "\\f");  break;
            default:
                if (c < 0x20) {
                    sb_printf(sb, "\\u%04x", c);
                } else {
                    sb_putc(sb, (char)c);
                }
        }
    }

    sb_putc(sb, '"');
}

/* logfmt value: bare when it has no separator/quote/control, else double-
 * quoted with \" and \\ escaped (Grafana/logfmt reader compatible). */
static void sb_put_logfmt_val(log_sbuf_t *sb, const char *s, size_t n)
{
    bool quote = (n == 0);

    for (size_t i = 0; i < n && !quote; i++) {
        char c = s[i];
        if (c == ' ' || c == '"' || c == '=' || (unsigned char)c < 0x20) {
            quote = true;
        }
    }

    if (!quote) {
        sb_write(sb, s, n);
        return;
    }

    sb_putc(sb, '"');
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '"' || s[i] == '\\') {
            sb_putc(sb, '\\');
        }
        sb_putc(sb, s[i]);
    }
    sb_putc(sb, '"');
}

static size_t format_iso8601(uint64_t ts_ns, char *out, size_t out_len)
{
    /* ISO-8601 UTC timestamp, ms precision. */
    time_t   sec = (time_t)(ts_ns / 1000000000ULL);
    uint32_t ms  = (uint32_t)((ts_ns % 1000000000ULL) / 1000000ULL);
    struct tm tm_buf;
#ifdef PHP_WIN32
    gmtime_s(&tm_buf, &sec);
#else
    gmtime_r(&sec, &tm_buf);
#endif

    int n = snprintf(out, out_len, "%04d-%02d-%02dT%02d:%02d:%02d.%03uZ",
                     tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
                     tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec, ms);

    return (n < 0) ? 0 : (size_t)n;
}

/* ANSI SGR codes; one table drives the pretty level badge colour (below). */
#define ANSI_RESET  "\x1b[0m"
#define ANSI_DIM    "\x1b[2m"
#define ANSI_RED    "\x1b[31m"
#define ANSI_GREEN  "\x1b[32m"
#define ANSI_YELLOW "\x1b[33m"

typedef enum {
    LOG_STYLE_PLAIN,
    LOG_STYLE_LOGFMT,
    LOG_STYLE_JSON,
    LOG_STYLE_PRETTY,   /* plain layout, keys dimmed with ANSI */
} log_style_t;

/* Single attribute-iteration helper shared by every formatter; the styles
 * differ only in the key/value separators and per-value rendering. plain,
 * logfmt and pretty lead each pair with a space (pretty dims the key); json
 * separates with commas and quotes keys. String values: plain/pretty are raw
 * (with "(null)"), logfmt conditionally quotes, json always JSON-escapes. */
static void sb_put_attrs(log_sbuf_t *sb, const http_log_record_t *rec,
                         log_style_t style)
{
    for (size_t i = 0; i < rec->attrs_count; i++) {
        const http_log_attr_t *a = &rec->attrs[i];

        if (style == LOG_STYLE_JSON) {
            if (i > 0) {
                sb_putc(sb, ',');
            }
            sb_put_json_str(sb, a->key, strlen(a->key));
            sb_putc(sb, ':');
        } else if (style == LOG_STYLE_PRETTY) {
            sb_putc(sb, ' ');
            sb_puts(sb, ANSI_DIM);
            sb_puts(sb, a->key);
            sb_puts(sb, ANSI_RESET);
            sb_putc(sb, '=');
        } else {
            sb_putc(sb, ' ');
            sb_puts(sb, a->key);
            sb_putc(sb, '=');
        }

        switch (a->type) {
            case HTTP_LOG_ATTR_STR: {
                const char *v = a->v.s;
                if (style == LOG_STYLE_JSON) {
                    sb_put_json_str(sb, v != NULL ? v : "", v != NULL ? strlen(v) : 0);
                } else if (style == LOG_STYLE_LOGFMT) {
                    sb_put_logfmt_val(sb, v != NULL ? v : "", v != NULL ? strlen(v) : 0);
                } else {
                    sb_puts(sb, v != NULL ? v : "(null)");
                }
                break;
            }
            case HTTP_LOG_ATTR_I64:
                sb_printf(sb, "%lld", (long long)a->v.i64);
                break;
            case HTTP_LOG_ATTR_U64:
                sb_printf(sb, "%llu", (unsigned long long)a->v.u64);
                break;
            case HTTP_LOG_ATTR_BOOL:
                sb_puts(sb, a->v.b ? "true" : "false");
                break;
            case HTTP_LOG_ATTR_F64:
                sb_printf(sb, "%g", a->v.f64);
                break;
        }
    }
}

size_t http_log_format_plain(const http_log_record_t *rec,
                             char *buf, size_t buf_len, void *ud)
{
    (void)ud;

    if (buf_len < 2) {
        return 0;
    }

    log_sbuf_t sb;
    sb_init(&sb, buf, buf_len);

    char ts[40];
    format_iso8601(rec->timestamp_ns, ts, sizeof ts);

    sb_puts(&sb, ts);
    sb_putc(&sb, ' ');
    sb_puts(&sb, severity_text(rec->severity));
    sb_putc(&sb, ' ');
    if (rec->body != NULL) {
        sb_write(&sb, rec->body, rec->body_len);
    }

    sb_put_attrs(&sb, rec, LOG_STYLE_PLAIN);
    sb_putc(&sb, '\n');

    return sb.len;
}

/* logfmt: ts=… level=… msg=… key=value …  (one line, parses in Grafana). */
size_t http_log_format_logfmt(const http_log_record_t *rec,
                              char *buf, size_t buf_len, void *ud)
{
    (void)ud;

    if (buf_len < 2) {
        return 0;
    }

    log_sbuf_t sb;
    sb_init(&sb, buf, buf_len);

    char ts[40];
    format_iso8601(rec->timestamp_ns, ts, sizeof ts);

    sb_printf(&sb, "ts=%s level=%s msg=", ts, severity_text(rec->severity));
    sb_put_logfmt_val(&sb, rec->body != NULL ? rec->body : "", rec->body_len);

    sb_put_attrs(&sb, rec, LOG_STYLE_LOGFMT);
    sb_putc(&sb, '\n');

    return sb.len;
}

/* JSON, one object per line, OTel Logs field names. Attributes is omitted when
 * empty; TraceId/SpanId only when the record carries a trace context. */
size_t http_log_format_json(const http_log_record_t *rec,
                            char *buf, size_t buf_len, void *ud)
{
    (void)ud;

    if (buf_len < 2) {
        return 0;
    }

    log_sbuf_t sb;
    sb_init(&sb, buf, buf_len);

    char ts[40];
    format_iso8601(rec->timestamp_ns, ts, sizeof ts);

    sb_printf(&sb, "{\"Timestamp\":\"%s\",\"SeverityNumber\":%d,\"SeverityText\":",
              ts, (int)rec->severity);
    sb_put_json_str(&sb, severity_text(rec->severity),
                    strlen(severity_text(rec->severity)));

    sb_puts(&sb, ",\"Body\":");
    sb_put_json_str(&sb, rec->body != NULL ? rec->body : "", rec->body_len);

    if (rec->attrs_count > 0) {
        sb_puts(&sb, ",\"Attributes\":{");
        sb_put_attrs(&sb, rec, LOG_STYLE_JSON);
        sb_putc(&sb, '}');
    }

    if (rec->has_trace) {
        sb_puts(&sb, ",\"TraceId\":\"");
        sb_put_hex(&sb, rec->trace_id, sizeof rec->trace_id);
        sb_puts(&sb, "\",\"SpanId\":\"");
        sb_put_hex(&sb, rec->span_id, sizeof rec->span_id);
        sb_putc(&sb, '"');
    }

    sb_putc(&sb, '}');
    sb_putc(&sb, '\n');

    return sb.len;
}

/* Fixed-width level badges + colours, indexed by pretty_level_idx. */
static const struct {
    const char *badge;   /* 5 columns, padded, so fields stay aligned */
    const char *color;
} pretty_level_style[] = {
    { "DEBUG", ANSI_DIM },
    { "INFO ", ANSI_GREEN },
    { "WARN ", ANSI_YELLOW },
    { "ERROR", ANSI_RED },
};

static int pretty_level_idx(http_log_severity_t s)
{
    switch (s) {
        case HTTP_LOG_DEBUG: return 0;
        case HTTP_LOG_WARN:  return 2;
        case HTTP_LOG_ERROR: return 3;
        case HTTP_LOG_INFO:
        default:             return 1;
    }
}

static void format_clock(uint64_t ts_ns, char *out, size_t out_len)
{
    time_t   sec = (time_t)(ts_ns / 1000000000ULL);
    uint32_t ms  = (uint32_t)((ts_ns % 1000000000ULL) / 1000000ULL);
    struct tm tm_buf;
#ifdef PHP_WIN32
    gmtime_s(&tm_buf, &sec);
#else
    gmtime_r(&sec, &tm_buf);
#endif

    snprintf(out, out_len, "%02d:%02d:%02d.%03u",
             tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec, ms);
}

/* Pretty console line: "HH:MM:SS.mmm  LEVEL  message  key=val …". Colour is
 * decided once at sink build and passed through `ud` (non-NULL = colour on);
 * with it off the output is plain text with no escape codes, safe for a file. */
size_t http_log_format_pretty(const http_log_record_t *rec,
                              char *buf, size_t buf_len, void *ud)
{
    const bool color = (ud != NULL);

    if (buf_len < 2) {
        return 0;
    }

    log_sbuf_t sb;
    sb_init(&sb, buf, buf_len);

    char clock[16];
    format_clock(rec->timestamp_ns, clock, sizeof clock);
    const int idx = pretty_level_idx(rec->severity);

    if (color) {
        sb_puts(&sb, ANSI_DIM);
    }
    sb_puts(&sb, clock);
    if (color) {
        sb_puts(&sb, ANSI_RESET);
    }

    sb_puts(&sb, "  ");
    if (color) {
        sb_puts(&sb, pretty_level_style[idx].color);
    }
    sb_puts(&sb, pretty_level_style[idx].badge);
    if (color) {
        sb_puts(&sb, ANSI_RESET);
    }

    sb_puts(&sb, "  ");
    if (rec->body != NULL) {
        sb_write(&sb, rec->body, rec->body_len);
    }

    sb_put_attrs(&sb, rec, color ? LOG_STYLE_PRETTY : LOG_STYLE_PLAIN);
    sb_putc(&sb, '\n');

    return sb.len;
}

/* Colour decision for a pretty sink, resolved once against the target fd:
 * NO_COLOR disables (https://no-color.org, wins for accessibility), else
 * CLICOLOR_FORCE enables, else colour follows whether the fd is a TTY. */
bool http_log_color_for_fd(int fd)
{
    if (getenv("NO_COLOR") != NULL) {
        return false;
    }

    if (getenv("CLICOLOR_FORCE") != NULL) {
        return true;
    }

#ifdef PHP_WIN32
    return _isatty(fd) != 0;
#else
    return isatty(fd) != 0;
#endif
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

/* Build one sink onto a zeroed slot: borrow the stream's async IO handle and
 * attach the coalescing writer. Returns false (sink left inactive, floor OFF)
 * on any failure. */
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

    php_stream *stream = NULL;
    php_stream_from_zval_no_verify(stream, stream_zv);

    if (stream == NULL) {
        return false;
    }

    /* Ask the stream for its async IO handle (true_async integration): the
     * stream get-or-creates a zend_async_io_t and owns its lifetime, so the
     * writer never touches a raw fd and any stream type that implements the
     * option (file, socket, pipe) works. We only borrow the handle to write
     * through and detach our callback at stop. */
    zend_async_io_t *io = NULL;
    int rc = php_stream_set_option(stream, PHP_STREAM_OPTION_ASYNC_IO, 0, &io);

    if (rc != PHP_STREAM_OPTION_RETURN_OK || io == NULL) {
        fprintf(stderr,
                "http_server: log stream has no async IO handle; "
                "sink disabled (use a file/socket/pipe stream)\n");
        return false;
    }

    http_log_writer_cb_t *cb = (http_log_writer_cb_t *)
        ZEND_ASYNC_EVENT_CALLBACK_EX(writer_complete_cb,
                                     sizeof(http_log_writer_cb_t));

    if (cb == NULL) {
        /* io belongs to the stream — leave it, just bail out. */
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
        efree(cb);   /* io belongs to the stream — leave it */
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

    /* The async IO handle belongs to the stream (get-or-create via
     * PHP_STREAM_OPTION_ASYNC_IO) — we only detached our writer callback
     * above. Dropping our stream ref lets the stream dispose the io when its
     * last ref goes; nothing for us to close here. */
    sink->async_io = NULL;

    if (sink->stream_set) {
        zval_ptr_dtor(&sink->stream_zv);
        sink->stream_set = false;
        ZVAL_UNDEF(&sink->stream_zv);
    }

    sink->severity_floor = HTTP_LOG_OFF;
}

void http_log_server_start_sinks(http_log_state_t *state,
                                  const http_log_sink_spec_t *specs, int n)
{
    if (state == NULL) {
        return;
    }

    /* Drain a stale config so re-activation doesn't leak. */
    if (state->sink_count > 0) {
        http_log_server_stop(state);
    }

    state->sink_count = 0;
    state->severity   = HTTP_LOG_OFF;

    if (n > HTTP_LOG_MAX_SINKS) {
        n = HTTP_LOG_MAX_SINKS;
    }

    for (int i = 0; i < n; i++) {
        if (http_log_sink_start(&state->sinks[state->sink_count],
                                specs[i].level, specs[i].formatter,
                                specs[i].formatter_ud, specs[i].stream_zv)) {
            state->sink_count++;
        }
    }

    http_log_state_refresh_gate(state);
}

void http_log_server_start(http_log_state_t *state,
                           http_log_severity_t severity,
                           zval *stream_zv)
{
    const http_log_sink_spec_t spec = {
        .level        = severity,
        .formatter    = http_log_format_plain,
        .formatter_ud = NULL,
        .stream_zv    = stream_zv,
    };

    http_log_server_start_sinks(state, &spec, 1);
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
