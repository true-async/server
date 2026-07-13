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

#include "php_http_server.h"          /* invalid_argument exception ce */
#include "log/http_log.h"
#include "core/reactor_pool.h"   /* the log thread runs on a 1-reactor pool */
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
/* winsock2.h must precede windows.h (project convention); it also
 * declares gethostname(), used for the syslog HEADER. */
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

/* Per-sink ring of formatted-but-unwritten bytes. Fixed capacity (a burst past
 * it drops, drop-counted); the writer drains it to the stream's async IO one
 * write at a time. Flush is kicked when the ring reaches the high-water mark or
 * the periodic flush timer ticks — so low-rate emits coalesce into fewer
 * syscalls while a burst never stalls the producer. CAP is a power of two so
 * the wrap is a mask. */
#define HTTP_LOG_RING_CAP          65536u   /* 64 KiB */
#define HTTP_LOG_FLUSH_HIGH_WATER  32768u   /* flush once >= 32 KiB buffered */
#define HTTP_LOG_FLUSH_INTERVAL_MS 200u

/* Ceiling on the total stop-time flush wait across all sinks; a wedged sink
 * past this leaks rather than pinning teardown. */
#define HTTP_LOG_STOP_DRAIN_BUDGET_MS 3000u

/* Distinct formatters cached per emit before fan-out. The built-in set is
 * plain/json/logfmt/pretty, so a real config never overflows; this also
 * bounds the emit-path stack to ~8 KiB of format buffers. */
#define HTTP_LOG_FMT_SLOTS 4

/* The sink's transport, owned by the log thread. A producer only reads `mode`
 * and reaches the ring list; everything else — the io, the write in flight, the
 * flush timer — is touched on the log thread alone. */
struct http_log_writer_cb {
    zend_async_event_callback_t  base;
    http_log_sink_t             *sink;
    zend_async_io_req_t         *active_req;
    char                        *active_buf;
    /* Record framing: applied by the producer as it appends, and decides the
     * write granularity on drain (DGRAM stores a u32 length header before each
     * record, so one record maps to exactly one write / one datagram). */
    http_log_write_mode_t        mode;

    /* One lock-free ring per producer thread (log_prod_t). A producer appends
     * to its own ring and never blocks; the log thread drains them all. The
     * list is only ever prepended to, under g_log_lock. */
    struct log_prod             *producers;

    /* Set by the producer that filled a ring past the high-water mark, cleared
     * by the log thread when it starts a drain — bounds the wake-ups to one in
     * flight per sink instead of one per record. */
    zend_atomic_bool             kick_pending;

    /* Stop handshake: the stopping thread sets `closing` and waits on
     * `stop_event` (a trigger it owns, fired cross-thread by the log thread);
     * the log thread finishes the drain and tears the transport down. */
    zend_atomic_bool             closing;
    zend_async_event_t          *stop_event;


    /* Periodic flush timer (lifetime of the sink): coalesces low-rate emits into
     * fewer writes, and is the safety net when a wake-up could not be posted. */
    zend_async_event_t          *flush_timer;
    zend_async_event_callback_t *flush_timer_cb;
};

/* Flush-timer callback: points back to the writer it drains. */
typedef struct {
    zend_async_event_callback_t  base;
    http_log_writer_cb_t        *writer;
} http_log_flush_timer_cb_t;

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

/* Terminate a record with its trailing newline — even when the record overflowed
 * the buffer. On a plain STREAM sink the '\n' is the only record separator, so a
 * truncated line that lost it would merge with the next record (an attacker with
 * a long request target could hide an unrelated request's log); force the
 * separator into the last byte rather than drop it. */
static void sb_end_line(log_sbuf_t *sb)
{
    if (sb->len + 1 < sb->cap) {
        sb->buf[sb->len++] = '\n';
        sb->buf[sb->len] = '\0';
        return;
    }

    if (sb->cap >= 2) {
        sb->buf[sb->cap - 2] = '\n';   /* last emitted byte (buf[len] is the NUL) */
        sb->len = sb->cap - 1;
        sb->buf[sb->cap - 1] = '\0';
    }
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

/* Write a value into a text line (plain/pretty/template/syslog), escaping
 * control bytes as \xXX. Request-derived fields — method, path — reach here
 * unfiltered, and a raw control byte would let a client forge a log line or,
 * for the ANSI-coloured pretty formatter written to a terminal, inject an
 * escape sequence. json/logfmt have their own quoting and do not use this. */
static void sb_put_text_safe(log_sbuf_t *sb, const char *s, size_t n)
{
    static const char hexd[] = "0123456789abcdef";

    for (size_t i = 0; i < n; i++) {
        const unsigned char c = (unsigned char)s[i];

        if (c < 0x20 || c == 0x7f) {
            sb_puts(sb, "\\x");
            sb_putc(sb, hexd[c >> 4]);
            sb_putc(sb, hexd[c & 0x0f]);
        } else {
            sb_putc(sb, (char)c);
        }
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

/* ISO-8601 UTC timestamp, ms precision: "YYYY-MM-DDTHH:MM:SS.mmmZ" (24
 * bytes). The gmtime+snprintf for the second part is cached per thread —
 * under an access-logged load thousands of records share one second, so
 * the common case is a 20-byte memcpy plus three ms digits. */
#define ISO8601_LEN 24

static size_t format_iso8601(uint64_t ts_ns, char *out, size_t out_len)
{
    ZEND_TLS time_t cached_sec = (time_t)-1;
    ZEND_TLS char   cached_prefix[21];   /* "YYYY-MM-DDTHH:MM:SS." + NUL */

    if (out_len < ISO8601_LEN + 1) {
        if (out_len > 0) {
            out[0] = '\0';
        }
        return 0;
    }

    const time_t   sec = (time_t)(ts_ns / 1000000000ULL);
    const uint32_t ms  = (uint32_t)((ts_ns % 1000000000ULL) / 1000000ULL);

    if (sec != cached_sec) {
        struct tm tm_buf;
#ifdef PHP_WIN32
        gmtime_s(&tm_buf, &sec);
#else
        gmtime_r(&sec, &tm_buf);
#endif
        snprintf(cached_prefix, sizeof cached_prefix,
                 "%04d-%02d-%02dT%02d:%02d:%02d.",
                 tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
                 tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec);
        cached_sec = sec;
    }

    memcpy(out, cached_prefix, 20);
    out[20] = (char)('0' + ms / 100);
    out[21] = (char)('0' + (ms / 10) % 10);
    out[22] = (char)('0' + ms % 10);
    out[23] = 'Z';
    out[24] = '\0';

    return ISO8601_LEN;
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
                } else if (v != NULL) {
                    sb_put_text_safe(sb, v, strlen(v));
                } else {
                    sb_puts(sb, "(null)");
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
        sb_put_text_safe(&sb, rec->body, rec->body_len);
    }

    sb_put_attrs(&sb, rec, LOG_STYLE_PLAIN);
    sb_end_line(&sb);

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
    sb_end_line(&sb);

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
    sb_end_line(&sb);

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

/* "HH:MM:SS.mmm" — the time-of-day slice of the ISO-8601 timestamp. */
static void format_clock(uint64_t ts_ns, char *out, size_t out_len)
{
    char ts[40];
    const size_t n = format_iso8601(ts_ns, ts, sizeof ts);

    if (n < 23 || out_len < 13) {
        if (out_len > 0) {
            out[0] = '\0';
        }
        return;
    }

    memcpy(out, ts + 11, 12);   /* skip "YYYY-MM-DDT" */
    out[12] = '\0';
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
        sb_put_text_safe(&sb, rec->body, rec->body_len);
    }

    sb_put_attrs(&sb, rec, color ? LOG_STYLE_PRETTY : LOG_STYLE_PLAIN);
    sb_end_line(&sb);

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

/* Compiled user template: literal runs and placeholders as a flat segment
 * list over one private copy of the template text. Compiled once at sink
 * build; the render is a straight walk with no parsing or allocation. */

#define LOG_TMPL_MAX_SEGS 24

typedef enum {
    TMPL_LIT,     /* literal bytes (also unknown placeholders, verbatim) */
    TMPL_TS,      /* len == 0 → ISO-8601; else a date()-style pattern */
    TMPL_LEVEL,
    TMPL_MSG,
    TMPL_ATTRS,
    TMPL_TRACE,
    TMPL_SPAN,
} log_tmpl_kind_t;

typedef struct {
    uint8_t  kind;
    uint16_t off;    /* into log_tmpl_t.text */
    uint16_t len;
} log_tmpl_seg_t;

typedef struct {
    int            nsegs;
    log_tmpl_seg_t segs[LOG_TMPL_MAX_SEGS];
    char           text[];
} log_tmpl_t;

static bool tmpl_push(log_tmpl_t *t, log_tmpl_kind_t kind,
                      size_t off, size_t len)
{
    if (t->nsegs >= LOG_TMPL_MAX_SEGS) {
        return false;
    }

    t->segs[t->nsegs].kind = (uint8_t)kind;
    t->segs[t->nsegs].off  = (uint16_t)off;
    t->segs[t->nsegs].len  = (uint16_t)len;
    t->nsegs++;
    return true;
}

static const struct { const char *kw; log_tmpl_kind_t kind; } tmpl_fields[] = {
    { "ts", TMPL_TS },       { "level", TMPL_LEVEL }, { "msg", TMPL_MSG },
    { "attrs", TMPL_ATTRS }, { "trace", TMPL_TRACE }, { "span", TMPL_SPAN },
};

void *http_log_template_parse(const char *tmpl, size_t len)
{
    if (len == 0 || len > HTTP_LOG_TEMPLATE_MAX) {
        return NULL;
    }

    log_tmpl_t *t = emalloc(sizeof *t + len);
    t->nsegs = 0;
    memcpy(t->text, tmpl, len);

    bool   ok = true;
    size_t i  = 0;

    while (ok && i < len) {
        const size_t lit = i;
        while (i < len && t->text[i] != '{') {
            i++;
        }
        if (i > lit) {
            ok = tmpl_push(t, TMPL_LIT, lit, i - lit);
        }
        if (!ok || i >= len) {
            break;
        }

        size_t close = i + 1;
        while (close < len && t->text[close] != '}') {
            close++;
        }
        if (close >= len) {   /* unmatched '{' — the rest is literal */
            ok = tmpl_push(t, TMPL_LIT, i, len - i);
            break;
        }

        const char *name = t->text + i + 1;
        size_t      nlen = close - (i + 1);
        size_t      colon = 0;
        while (colon < nlen && name[colon] != ':') {
            colon++;
        }

        log_tmpl_kind_t kind  = TMPL_LIT;
        bool            known = false;

        for (size_t k = 0; k < sizeof tmpl_fields / sizeof tmpl_fields[0]; k++) {
            if (strlen(tmpl_fields[k].kw) == colon
                && memcmp(tmpl_fields[k].kw, name, colon) == 0) {
                kind  = tmpl_fields[k].kind;
                known = true;
                break;
            }
        }

        if (!known) {
            ok = tmpl_push(t, TMPL_LIT, i, close - i + 1);   /* verbatim */
        } else if (kind == TMPL_TS && colon < nlen) {
            ok = tmpl_push(t, TMPL_TS, i + 1 + colon + 1, nlen - colon - 1);
        } else {
            ok = tmpl_push(t, kind, 0, 0);
        }

        i = close + 1;
    }

    if (!ok) {
        efree(t);
        return NULL;
    }

    return t;
}

/* Render the timestamp with a PHP date()-style pattern (subset: Y y m d H i
 * s v); any other character passes through literally. */
static void tmpl_put_ts(log_sbuf_t *sb, uint64_t ts_ns,
                        const char *pat, size_t n)
{
    const time_t   sec = (time_t)(ts_ns / 1000000000ULL);
    const uint32_t ms  = (uint32_t)((ts_ns % 1000000000ULL) / 1000000ULL);
    struct tm tm_buf;
#ifdef PHP_WIN32
    gmtime_s(&tm_buf, &sec);
#else
    gmtime_r(&sec, &tm_buf);
#endif

    for (size_t i = 0; i < n; i++) {
        switch (pat[i]) {
            case 'Y': sb_printf(sb, "%04d", tm_buf.tm_year + 1900); break;
            case 'y': sb_printf(sb, "%02d", (tm_buf.tm_year + 1900) % 100); break;
            case 'm': sb_printf(sb, "%02d", tm_buf.tm_mon + 1); break;
            case 'd': sb_printf(sb, "%02d", tm_buf.tm_mday); break;
            case 'H': sb_printf(sb, "%02d", tm_buf.tm_hour); break;
            case 'i': sb_printf(sb, "%02d", tm_buf.tm_min); break;
            case 's': sb_printf(sb, "%02d", tm_buf.tm_sec); break;
            case 'v': sb_printf(sb, "%03u", ms); break;
            default:  sb_putc(sb, pat[i]);
        }
    }
}

size_t http_log_format_template(const http_log_record_t *rec,
                                char *buf, size_t buf_len, void *ud)
{
    if (ud == NULL) {
        return http_log_format_plain(rec, buf, buf_len, NULL);
    }

    if (buf_len < 2) {
        return 0;
    }

    const log_tmpl_t *t = (const log_tmpl_t *)ud;
    log_sbuf_t sb;
    sb_init(&sb, buf, buf_len);

    for (int i = 0; i < t->nsegs; i++) {
        const log_tmpl_seg_t *seg = &t->segs[i];

        switch ((log_tmpl_kind_t)seg->kind) {
            case TMPL_LIT:
                sb_write(&sb, t->text + seg->off, seg->len);
                break;
            case TMPL_TS:
                if (seg->len == 0) {
                    char ts[40];
                    format_iso8601(rec->timestamp_ns, ts, sizeof ts);
                    sb_puts(&sb, ts);
                } else {
                    tmpl_put_ts(&sb, rec->timestamp_ns,
                                t->text + seg->off, seg->len);
                }
                break;
            case TMPL_LEVEL:
                sb_puts(&sb, severity_text(rec->severity));
                break;
            case TMPL_MSG:
                if (rec->body != NULL) {
                    sb_put_text_safe(&sb, rec->body, rec->body_len);
                }
                break;
            case TMPL_ATTRS:
                sb_put_attrs(&sb, rec, LOG_STYLE_PLAIN);
                break;
            case TMPL_TRACE:
                if (rec->has_trace) {
                    sb_put_hex(&sb, rec->trace_id, sizeof rec->trace_id);
                }
                break;
            case TMPL_SPAN:
                if (rec->has_trace) {
                    sb_put_hex(&sb, rec->span_id, sizeof rec->span_id);
                }
                break;
        }
    }

    sb_end_line(&sb);
    return sb.len;
}

#define HTTP_LOG_SYSLOG_APPNAME "php-http-server"

/* RFC 5424 facility keyword → numeric code, or -1 if unknown. */
int http_log_syslog_facility(const char *name, size_t len)
{
    static const struct { const char *kw; int code; } table[] = {
        { "kern", 0 }, { "user", 1 }, { "mail", 2 }, { "daemon", 3 },
        { "auth", 4 }, { "syslog", 5 }, { "lpr", 6 }, { "news", 7 },
        { "uucp", 8 }, { "cron", 9 }, { "authpriv", 10 }, { "ftp", 11 },
        { "local0", 16 }, { "local1", 17 }, { "local2", 18 }, { "local3", 19 },
        { "local4", 20 }, { "local5", 21 }, { "local6", 22 }, { "local7", 23 },
    };

    for (size_t i = 0; i < sizeof table / sizeof table[0]; i++) {
        if (strlen(table[i].kw) == len && memcmp(table[i].kw, name, len) == 0) {
            return table[i].code;
        }
    }

    return -1;
}

uint8_t http_log_category_mask(const char *name, size_t len)
{
    static const struct { const char *kw; uint8_t mask; } table[] = {
        { "app",    HTTP_LOG_CAT_APP },
        { "access", HTTP_LOG_CAT_ACCESS },
        { "all",    HTTP_LOG_CAT_APP | HTTP_LOG_CAT_ACCESS },
    };

    for (size_t i = 0; i < sizeof table / sizeof table[0]; i++) {
        if (strlen(table[i].kw) == len && memcmp(table[i].kw, name, len) == 0) {
            return table[i].mask;
        }
    }

    return 0;
}

/* OTel severity → RFC 5424 syslog severity (0..7). */
static int syslog_severity(http_log_severity_t s)
{
    switch (s) {
        case HTTP_LOG_ERROR: return 3;   /* Error */
        case HTTP_LOG_WARN:  return 4;   /* Warning */
        case HTTP_LOG_DEBUG: return 7;   /* Debug */
        case HTTP_LOG_INFO:
        default:             return 6;   /* Informational */
    }
}

/* Local hostname for the syslog HEADER. "-" (NILVALUE) if unavailable.
 * Resolved once at MINIT (single-threaded) — formatters then read it from any
 * worker thread with no race. */
static char syslog_host[256] = "-";

static void syslog_hostname_resolve(void)
{
    if (gethostname(syslog_host, sizeof syslog_host - 1) != 0
        || syslog_host[0] == '\0') {
        syslog_host[0] = '-';
        syslog_host[1] = '\0';
    }
}

/* Bare RFC 5424 message: "<PRI>1 TIMESTAMP HOST APP PROCID - - MSG". PRI packs
 * the facility (via `ud`, defaulting to user=1) and the mapped severity; MSG
 * carries the body + attrs. Record framing is the transport's job (see
 * http_log_write_mode_t): octet-count on a stream, one datagram on UDP/unix. */
size_t http_log_format_syslog(const http_log_record_t *rec,
                              char *buf, size_t buf_len, void *ud)
{
    int facility = (int)(intptr_t)ud;
    if (facility < 0 || facility > 23) {
        facility = 1;   /* user-level */
    }

    const int pri = facility * 8 + syslog_severity(rec->severity);

    char ts[40];
    format_iso8601(rec->timestamp_ns, ts, sizeof ts);

    log_sbuf_t sb;
    sb_init(&sb, buf, buf_len);

    sb_printf(&sb, "<%d>1 %s %s %s %ld - - ",
              pri, ts, syslog_host, HTTP_LOG_SYSLOG_APPNAME,
              (long)getpid());
    if (rec->body != NULL) {
        sb_put_text_safe(&sb, rec->body, rec->body_len);
    }
    sb_put_attrs(&sb, rec, LOG_STYLE_PLAIN);

    return sb.len;
}

/* ------------------------------------------------------------------------ */
/* Sink-type / formatter registry. Built-ins register in http_log_minit;
 * plugin extensions add theirs from their own MINIT (single-threaded, so no
 * locking). The registry stores the caller's static def pointers. */

#define HTTP_LOG_REG_SLOTS 16

static const http_log_formatter_def_t *g_formatters[HTTP_LOG_REG_SLOTS];
static int                             g_formatter_count = 0;
static const http_log_sink_type_t     *g_sink_types[HTTP_LOG_REG_SLOTS];
static int                             g_sink_type_count = 0;

const http_log_formatter_def_t *http_log_formatter_by_name(const char *name,
                                                           size_t len)
{
    for (int i = 0; i < g_formatter_count; i++) {
        const char *kw = g_formatters[i]->name;

        if (strlen(kw) == len && memcmp(kw, name, len) == 0) {
            return g_formatters[i];
        }
    }

    return NULL;
}

const http_log_sink_type_t *http_log_sink_type_by_name(const char *name,
                                                       size_t len)
{
    for (int i = 0; i < g_sink_type_count; i++) {
        const char *kw = g_sink_types[i]->name;

        if (strlen(kw) == len && memcmp(kw, name, len) == 0) {
            return g_sink_types[i];
        }
    }

    return NULL;
}

bool http_log_register_formatter(const http_log_formatter_def_t *def)
{
    if (def == NULL || def->name == NULL || def->fn == NULL
        || g_formatter_count >= HTTP_LOG_REG_SLOTS
        || http_log_formatter_by_name(def->name, strlen(def->name)) != NULL) {
        return false;
    }

    g_formatters[g_formatter_count++] = def;
    return true;
}

bool http_log_register_sink_type(const http_log_sink_type_t *type)
{
    if (type == NULL || type->name == NULL
        || type->open == NULL
        || g_sink_type_count >= HTTP_LOG_REG_SLOTS
        || http_log_sink_type_by_name(type->name, strlen(type->name)) != NULL) {
        return false;
    }

    g_sink_types[g_sink_type_count++] = type;
    return true;
}

void http_log_formatter_names(char *buf, size_t cap)
{
    log_sbuf_t sb;
    sb_init(&sb, buf, cap);

    for (int i = 0; i < g_formatter_count; i++) {
        if (i > 0) {
            sb_putc(&sb, '|');
        }
        sb_puts(&sb, g_formatters[i]->name);
    }
}

void http_log_sink_type_names(char *buf, size_t cap)
{
    log_sbuf_t sb;
    sb_init(&sb, buf, cap);

    for (int i = 0; i < g_sink_type_count; i++) {
        if (i > 0) {
            sb_putc(&sb, '|');
        }
        sb_puts(&sb, g_sink_types[i]->name);
    }
}

/* --- built-in formatter defs --- */

/* pretty carries the colour decision in ud, resolved once against the
 * sink's stream fd (NO_COLOR / CLICOLOR_FORCE / isatty). */
static void *formatter_ud_pretty(HashTable *spec, zval *stream_zv)
{
    (void)spec;

    php_stream *s = NULL;

    if (stream_zv != NULL) {
        php_stream_from_zval_no_verify(s, stream_zv);
    }
    if (s == NULL) {
        return NULL;
    }

    int fd = -1;
    if (php_stream_cast(s, PHP_STREAM_AS_FD | PHP_STREAM_CAST_INTERNAL,
                        (void *)&fd, 0) != SUCCESS || fd < 0) {
        return NULL;
    }

    return http_log_color_for_fd(fd) ? (void *)1 : NULL;
}

/* syslog carries the facility code in ud (default user=1). */
static void *formatter_ud_syslog(HashTable *spec, zval *stream_zv)
{
    (void)stream_zv;

    int   fac  = 1;
    zval *zfac = spec != NULL
        ? zend_hash_str_find(spec, "facility", sizeof("facility") - 1) : NULL;

    if (zfac != NULL && Z_TYPE_P(zfac) == IS_STRING) {
        const int f = http_log_syslog_facility(Z_STRVAL_P(zfac), Z_STRLEN_P(zfac));
        if (f >= 0) {
            fac = f;
        }
    }

    return (void *)(intptr_t)fac;
}

/* template compiles its 'template' spec key once at sink build; the compiled
 * block is owned by the sink (freed via free_ud at stop). */
static bool formatter_validate_template(HashTable *spec)
{
    zval *zt = zend_hash_str_find(spec, "template", sizeof("template") - 1);

    if (zt == NULL || Z_TYPE_P(zt) != IS_STRING || Z_STRLEN_P(zt) == 0
        || Z_STRLEN_P(zt) > HTTP_LOG_TEMPLATE_MAX) {
        zend_throw_exception_ex(http_server_invalid_argument_exception_ce, 0,
            "setLogSinks(): format 'template' requires a 'template' string "
            "(1..%d bytes)", HTTP_LOG_TEMPLATE_MAX);
        return false;
    }

    void *t = http_log_template_parse(Z_STRVAL_P(zt), Z_STRLEN_P(zt));

    if (t == NULL) {
        zend_throw_exception_ex(http_server_invalid_argument_exception_ce, 0,
            "setLogSinks(): 'template' has too many segments (max %d)",
            LOG_TMPL_MAX_SEGS);
        return false;
    }

    efree(t);
    return true;
}

static void *formatter_ud_template(HashTable *spec, zval *stream_zv)
{
    (void)stream_zv;

    zval *zt = spec != NULL
        ? zend_hash_str_find(spec, "template", sizeof("template") - 1) : NULL;

    if (zt == NULL || Z_TYPE_P(zt) != IS_STRING) {
        return NULL;   /* render falls back to plain */
    }

    return http_log_template_parse(Z_STRVAL_P(zt), Z_STRLEN_P(zt));
}

static void formatter_ud_efree(void *ud)
{
    efree(ud);
}

static const http_log_formatter_def_t fmt_plain    = { "plain",    http_log_format_plain,    NULL, NULL, NULL };
static const http_log_formatter_def_t fmt_logfmt   = { "logfmt",   http_log_format_logfmt,   NULL, NULL, NULL };
static const http_log_formatter_def_t fmt_json     = { "json",     http_log_format_json,     NULL, NULL, NULL };
static const http_log_formatter_def_t fmt_pretty   = { "pretty",   http_log_format_pretty,   NULL, formatter_ud_pretty, NULL };
static const http_log_formatter_def_t fmt_syslog   = { "syslog",   http_log_format_syslog,   NULL, formatter_ud_syslog, NULL };
static const http_log_formatter_def_t fmt_template = { "template", http_log_format_template,
                                                       formatter_validate_template,
                                                       formatter_ud_template,
                                                       formatter_ud_efree };

/* --- built-in sink types --- */

static bool sink_validate_stream(HashTable *spec)
{
    zval       *zs = zend_hash_str_find(spec, "stream", sizeof("stream") - 1);
    php_stream *st = NULL;

    if (zs != NULL && Z_TYPE_P(zs) == IS_RESOURCE) {
        php_stream_from_zval_no_verify(st, zs);
    }

    if (st == NULL) {
        zend_throw_exception(http_server_invalid_argument_exception_ce,
            "setLogSinks(): type 'stream' requires a php_stream 'stream' resource", 0);
        return false;
    }

    return true;
}

static bool sink_open_stream(HashTable *spec, zval *out,
                             http_log_write_mode_t *mode)
{
    (void)mode;

    zval *zs = zend_hash_str_find(spec, "stream", sizeof("stream") - 1);

    if (zs == NULL || Z_TYPE_P(zs) != IS_RESOURCE) {
        return false;
    }

    ZVAL_COPY(out, zs);
    return true;
}

static bool sink_open_php_dev(const char *dev, zval *out)
{
    php_stream *s = php_stream_open_wrapper(dev, "wb", 0, NULL);

    if (s == NULL) {
        return false;
    }

    php_stream_to_zval(s, out);
    return true;
}

static bool sink_open_stdout(HashTable *spec, zval *out,
                             http_log_write_mode_t *mode)
{
    (void)spec;
    (void)mode;
    return sink_open_php_dev("php://stdout", out);
}

static bool sink_open_stderr(HashTable *spec, zval *out,
                             http_log_write_mode_t *mode)
{
    (void)spec;
    (void)mode;
    return sink_open_php_dev("php://stderr", out);
}

/* Syslog target scheme → writer mode: a TCP stream needs RFC 6587 octet
 * framing, a datagram transport (udp://, unix udg://) needs one record per
 * write. NULL when the scheme is unsupported. */
static const struct {
    const char            *scheme;
    size_t                 scheme_len;
    http_log_write_mode_t  mode;
} syslog_schemes[] = {
    { "tcp://", 6, HTTP_LOG_WRITE_STREAM_FRAMED },
    { "udp://", 6, HTTP_LOG_WRITE_DGRAM },
    { "udg://", 6, HTTP_LOG_WRITE_DGRAM },   /* unix datagram, e.g. /dev/log */
};

static bool syslog_target_mode(const zval *ztarget, http_log_write_mode_t *mode)
{
    if (ztarget == NULL || Z_TYPE_P(ztarget) != IS_STRING) {
        return false;
    }

    for (size_t i = 0; i < sizeof syslog_schemes / sizeof syslog_schemes[0]; i++) {
        if ((size_t)Z_STRLEN_P(ztarget) > syslog_schemes[i].scheme_len
            && memcmp(Z_STRVAL_P(ztarget), syslog_schemes[i].scheme,
                      syslog_schemes[i].scheme_len) == 0) {
            *mode = syslog_schemes[i].mode;
            return true;
        }
    }

    return false;
}

/* 'file' opens its own append-mode stream from a path — the type that works
 * under a worker pool, where a parent-opened 'stream' resource cannot cross
 * into worker threads (each worker's start_logging reopens the path). */
static bool sink_validate_file(HashTable *spec)
{
    zval *zp = zend_hash_str_find(spec, "path", sizeof("path") - 1);

    if (zp == NULL || Z_TYPE_P(zp) != IS_STRING || Z_STRLEN_P(zp) == 0) {
        zend_throw_exception(http_server_invalid_argument_exception_ce,
            "setLogSinks(): type 'file' requires a non-empty 'path' string", 0);
        return false;
    }

    return true;
}

static bool sink_open_file(HashTable *spec, zval *out,
                           http_log_write_mode_t *mode)
{
    (void)mode;

    zval *zp = zend_hash_str_find(spec, "path", sizeof("path") - 1);

    if (zp == NULL || Z_TYPE_P(zp) != IS_STRING) {
        return false;
    }

    php_stream *s = php_stream_open_wrapper(Z_STRVAL_P(zp), "ab",
                                            REPORT_ERRORS, NULL);

    if (s == NULL) {
        return false;
    }

    php_stream_to_zval(s, out);
    return true;
}

static bool sink_validate_syslog(HashTable *spec)
{
    zval *ztarget = zend_hash_str_find(spec, "target", sizeof("target") - 1);
    http_log_write_mode_t mode;

    if (!syslog_target_mode(ztarget, &mode)) {
        zend_throw_exception(http_server_invalid_argument_exception_ce,
            "setLogSinks(): type 'syslog' requires a 'tcp://host:port', "
            "'udp://host:port' or 'udg:///path' target", 0);
        return false;
    }

    zval *zfac = zend_hash_str_find(spec, "facility", sizeof("facility") - 1);

    if (zfac != NULL
        && (Z_TYPE_P(zfac) != IS_STRING
            || http_log_syslog_facility(Z_STRVAL_P(zfac), Z_STRLEN_P(zfac)) < 0)) {
        zend_throw_exception(http_server_invalid_argument_exception_ce,
            "setLogSinks(): syslog 'facility' must be a keyword (user, daemon, local0..7, …)", 0);
        return false;
    }

    return true;
}

static bool sink_open_syslog(HashTable *spec, zval *out,
                             http_log_write_mode_t *mode)
{
    zval *ztarget = zend_hash_str_find(spec, "target", sizeof("target") - 1);

    if (!syslog_target_mode(ztarget, mode)) {
        return false;
    }

    zend_string *xerr  = NULL;
    int          xcode = 0;
    php_stream  *s = php_stream_xport_create(
        Z_STRVAL_P(ztarget), Z_STRLEN_P(ztarget), 0,
        STREAM_XPORT_CLIENT | STREAM_XPORT_CONNECT,
        NULL, NULL, NULL, &xerr, &xcode);

    if (xerr != NULL) {
        zend_string_release(xerr);
    }
    if (s == NULL) {
        return false;   /* target unreachable — the sink is skipped */
    }

    php_stream_to_zval(s, out);
    return true;
}

static const http_log_sink_type_t sink_type_stream = { "stream", sink_validate_stream, sink_open_stream, NULL };
static const http_log_sink_type_t sink_type_file   = { "file",   sink_validate_file,   sink_open_file,   NULL };
static const http_log_sink_type_t sink_type_stdout = { "stdout", NULL, sink_open_stdout, NULL };
static const http_log_sink_type_t sink_type_stderr = { "stderr", NULL, sink_open_stderr, NULL };
static const http_log_sink_type_t sink_type_syslog = { "syslog", sink_validate_syslog, sink_open_syslog, &fmt_syslog };

/* ------------------------------------------------------------------------ */

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

/* ------------------------------------------------------------------------
 * The log thread
 *
 * Sinks are owned by ONE consumer thread: it holds the descriptor, the write in
 * flight and the flush timer, and it is the only thread that touches them. Every
 * other thread — pool workers, transport reactors, the parent — is a producer:
 * it formats a record on its own stack and copies the bytes into its OWN ring
 * for that sink. One writer, one reader per ring, so the hot path takes no lock
 * and no atomic read-modify-write; publishing the write index with a release
 * store is the whole synchronisation.
 *
 * This is what lets a transport reactor emit an access record at all: it has no
 * PHP context and must never touch a worker's descriptor, but it can copy bytes
 * into a ring. It also means one descriptor per sink instead of one per worker.
 * ------------------------------------------------------------------------- */

/* Bytes a single drain hands to one write. Bounds the copy-out buffer when many
 * producers have queued up at once. */
#define HTTP_LOG_WRITE_CHUNK_MAX (256u * 1024u)

/* Per (producer thread, sink) ring. head/tail are free-running counters, not
 * offsets: the difference is the fill level and wraps correctly, so no
 * empty-vs-full ambiguity and no spare byte. Only the producer advances head,
 * only the log thread advances tail. */
typedef struct log_prod {
    struct log_prod  *next;      /* other producers of the same sink */
    char             *buf;       /* persistent: crosses threads */
    zend_atomic_int   head;      /* producer publishes (release) */
    zend_atomic_int   tail;      /* log thread publishes (release) */
    zend_atomic_bool  dead;      /* sink stopped: stop appending, re-register */
    uint64_t          dropped;   /* producer-only: records lost to a full ring */
} log_prod_t;

static reactor_pool_t *g_log_pool  = NULL;   /* the log thread (1 reactor) */
static int             g_log_refs  = 0;      /* sinks that need it alive */
static MUTEX_T         g_log_lock  = NULL;   /* guards the two above + ring lists */

/* Producers cache their ring per sink; a stopped sink bumps the generation so a
 * stale cache entry is dropped instead of writing into a freed ring. */
#define HTTP_LOG_TLS_RINGS 16
static zend_atomic_int g_log_generation;

ZEND_TLS struct {
    const http_log_writer_cb_t *cb;
    log_prod_t                 *ring;
} tls_rings[HTTP_LOG_TLS_RINGS];
ZEND_TLS int tls_ring_count = 0;
ZEND_TLS int tls_ring_gen   = -1;

/* This thread's drop counter (its own counters slice), or NULL. */
ZEND_TLS uint64_t *tls_drop_counter = NULL;

void http_log_set_thread_drop_counter(uint64_t *counter)
{
    tls_drop_counter = counter;
}

static void writer_kick_next(http_log_writer_cb_t *cb);
static void writer_flush_timer_create(http_log_writer_cb_t *cb);
static void writer_flush_timer_destroy(http_log_writer_cb_t *cb);

/* The writer cb is embedded in the sink's transport, which writer_teardown
 * frees on the log thread — the event layer must not free it for us. */
static void writer_callback_dispose(zend_async_event_callback_t *callback,
                                    zend_async_event_t          *event)
{
    (void)callback;
    (void)event;
}

/* Start (or join) the log thread. Called with g_log_lock held. */
static bool log_thread_ref(void)
{
    if (g_log_pool == NULL) {
        g_log_pool = reactor_pool_create(1, 0);

        if (g_log_pool == NULL) {
            return false;
        }
    }

    g_log_refs++;
    return true;
}

static void log_thread_unref(void)
{
    if (--g_log_refs > 0 || g_log_pool == NULL) {
        return;
    }

    reactor_pool_t *const pool = g_log_pool;
    g_log_pool = NULL;
    reactor_pool_destroy(pool);   /* joins the thread */
}

/* Ring geometry. CAP is a power of two, so the mask is the index. */
static inline uint32_t ring_used(const log_prod_t *r)
{
    const uint32_t head = (uint32_t)zend_atomic_int_load_ex(&((log_prod_t *)r)->head);
    const uint32_t tail = (uint32_t)zend_atomic_int_load_ex(&((log_prod_t *)r)->tail);
    return head - tail;
}

/* ---- producer side (any thread: worker, reactor, parent) --------------- */

static log_prod_t *log_prod_ring_for(http_log_writer_cb_t *cb)
{
    const int gen = zend_atomic_int_load_ex(&g_log_generation);

    if (tls_ring_gen != gen) {
        tls_ring_gen   = gen;
        tls_ring_count = 0;   /* a sink stopped: every cached ring may be gone */
    }

    for (int i = 0; i < tls_ring_count; i++) {
        if (tls_rings[i].cb == cb) {
            return tls_rings[i].ring;
        }
    }

    log_prod_t *const r = pecalloc(1, sizeof(*r), 1);
    r->buf = pemalloc(HTTP_LOG_RING_CAP, 1);
    ZEND_ATOMIC_INT_INIT(&r->head, 0);
    ZEND_ATOMIC_INT_INIT(&r->tail, 0);
    ZEND_ATOMIC_BOOL_INIT(&r->dead, false);

    tsrm_mutex_lock(g_log_lock);
    r->next = cb->producers;
    cb->producers = r;              /* published before any head store below */
    tsrm_mutex_unlock(g_log_lock);

    if (tls_ring_count < HTTP_LOG_TLS_RINGS) {
        tls_rings[tls_ring_count].cb   = cb;
        tls_rings[tls_ring_count].ring = r;
        tls_ring_count++;
    }

    return r;
}

/* Copy `len` bytes into the ring at the (unmasked) counter position. */
static void ring_put(log_prod_t *r, uint32_t at, const char *src, uint32_t len)
{
    const uint32_t pos   = at & (HTTP_LOG_RING_CAP - 1);
    const uint32_t first = HTTP_LOG_RING_CAP - pos;

    if (len <= first) {
        memcpy(r->buf + pos, src, len);
    } else {
        memcpy(r->buf + pos, src, first);
        memcpy(r->buf, src + first, len - first);
    }
}

static void ring_copy_out(const log_prod_t *r, uint32_t at, char *dst, uint32_t len)
{
    const uint32_t pos   = at & (HTTP_LOG_RING_CAP - 1);
    const uint32_t first = HTTP_LOG_RING_CAP - pos;

    if (len <= first) {
        memcpy(dst, r->buf + pos, len);
    } else {
        memcpy(dst, r->buf + pos, first);
        memcpy(dst + first, r->buf, len - first);
    }
}

/* Append one record (optional frame prefix + payload) atomically: a burst past
 * the ring's capacity drops the whole record, never half of it. The head store
 * is what publishes it — the log thread never sees a partial record. */
static void log_prod_append(http_log_writer_cb_t *cb, log_prod_t *r,
                            const char *pre, size_t pre_len,
                            const char *src, size_t len)
{
    if (len == 0) {
        return;
    }

    const uint32_t head = (uint32_t)zend_atomic_int_load_ex(&r->head);
    const uint32_t used = head - (uint32_t)zend_atomic_int_load_ex(&r->tail);
    const uint32_t need = (uint32_t)(pre_len + len);

    if (need > HTTP_LOG_RING_CAP - used) {
        r->dropped++;

        if (cb->sink != NULL) {
            cb->sink->dropped_total++;
        }

        if (tls_drop_counter != NULL) {
            (*tls_drop_counter)++;   /* our own slice: no other thread writes it */
        }

        emit_fallback_stderr(cb->sink, "ring overflow");
        return;
    }

    if (pre_len > 0) {
        ring_put(r, head, pre, (uint32_t)pre_len);
    }

    ring_put(r, head + (uint32_t)pre_len, src, (uint32_t)len);
    zend_atomic_int_store_ex(&r->head, (int)(head + need));   /* publish */

    if (used + need < HTTP_LOG_FLUSH_HIGH_WATER) {
        return;   /* below the high-water mark: the flush timer will take it */
    }

    /* One wake-up in flight per sink; the log thread clears the flag when it
     * starts draining. A full mailbox is not a loss — the 200 ms timer on the
     * log thread drains regardless. */
    if (!zend_atomic_bool_exchange_ex(&cb->kick_pending, true)) {
        if (!reactor_pool_post_exec(g_log_pool, 0, (reactor_exec_fn)writer_kick_next, cb)) {
            zend_atomic_bool_store_ex(&cb->kick_pending, false);
        }
    }
}

/* Hand one already-formatted record to a sink: frame it per the writer mode and
 * append to this thread's ring. Never blocks, never suspends, never touches the
 * descriptor — that belongs to the log thread. */
static void http_log_sink_write(http_log_sink_t *sink, const char *buf, size_t len)
{
    http_log_writer_cb_t *cb = sink->writer_cb;

    if (cb == NULL || sink->async_io == NULL
        || zend_atomic_bool_load_ex(&cb->closing)) {
        return;
    }

    log_prod_t *const r = log_prod_ring_for(cb);

    if (UNEXPECTED(r == NULL) || zend_atomic_bool_load_ex(&r->dead)) {
        return;
    }

    switch (cb->mode) {
        case HTTP_LOG_WRITE_STREAM_FRAMED: {
            char pre[24];   /* RFC 6587 octet count: "LEN SP" */
            const int pn = snprintf(pre, sizeof pre, "%zu ", len);
            log_prod_append(cb, r, pre, (size_t)pn, buf, len);
            break;
        }
        case HTTP_LOG_WRITE_DGRAM: {
            const uint32_t rec_len = (uint32_t)len;
            log_prod_append(cb, r, (const char *)&rec_len, sizeof rec_len, buf, len);
            break;
        }
        case HTTP_LOG_WRITE_STREAM:
        default:
            log_prod_append(cb, r, NULL, 0, buf, len);
    }
}

/* ---- consumer side (log thread only) ----------------------------------- */

static void writer_teardown(http_log_writer_cb_t *cb);

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
}

/* Pull the next write out of the producer rings. STREAM modes coalesce whatever
 * every producer has queued into one write; DGRAM sends exactly one record, so
 * each record travels as its own datagram (the completion chains the next). */
static void writer_kick_next(http_log_writer_cb_t *cb)
{
    zend_atomic_bool_store_ex(&cb->kick_pending, false);

    if (cb->active_req != NULL) {
        return;   /* a write is in flight; its completion re-kicks */
    }

    if (cb->sink == NULL || cb->sink->async_io == NULL) {
        if (zend_atomic_bool_load_ex(&cb->closing)) {
            writer_teardown(cb);
        }

        return;
    }

    char    *out   = NULL;
    uint32_t chunk = 0;

    if (cb->mode == HTTP_LOG_WRITE_DGRAM) {
        for (log_prod_t *r = cb->producers; r != NULL; r = r->next) {
            if (ring_used(r) < sizeof(uint32_t)) {
                continue;
            }

            const uint32_t tail = (uint32_t)zend_atomic_int_load_ex(&r->tail);
            uint32_t rec_len;
            ring_copy_out(r, tail, (char *)&rec_len, sizeof rec_len);

            out   = emalloc(rec_len);
            chunk = rec_len;
            ring_copy_out(r, tail + (uint32_t)sizeof rec_len, out, rec_len);
            zend_atomic_int_store_ex(&r->tail,
                                     (int)(tail + (uint32_t)sizeof rec_len + rec_len));
            break;
        }
    } else {
        uint32_t total = 0;

        for (log_prod_t *r = cb->producers; r != NULL; r = r->next) {
            total += ring_used(r);
        }

        if (total > HTTP_LOG_WRITE_CHUNK_MAX) {
            total = HTTP_LOG_WRITE_CHUNK_MAX;
        }

        if (total > 0) {
            out = emalloc(total);

            for (log_prod_t *r = cb->producers; r != NULL && chunk < total; r = r->next) {
                uint32_t used = ring_used(r);

                if (used == 0) {
                    continue;
                }

                if (used > total - chunk) {
                    used = total - chunk;   /* rest rides the next write */
                }

                const uint32_t tail = (uint32_t)zend_atomic_int_load_ex(&r->tail);
                ring_copy_out(r, tail, out + chunk, used);
                zend_atomic_int_store_ex(&r->tail, (int)(tail + used));
                chunk += used;
            }
        }
    }

    if (chunk == 0) {
        if (out != NULL) {
            efree(out);
        }

        if (zend_atomic_bool_load_ex(&cb->closing)) {
            writer_teardown(cb);   /* drained: finish the stop handshake */
        }

        return;
    }

    cb->active_buf = out;
    cb->active_req = ZEND_ASYNC_IO_WRITE(cb->sink->async_io, out, chunk);

    if (UNEXPECTED(cb->active_req == NULL)) {
        efree(out);
        cb->active_buf = NULL;
        cb->sink->dropped_total++;
        emit_fallback_stderr(cb->sink, "async write submit failed");
    }
}

/* Log thread: the drain is done and a stop is pending — release the transport
 * and hand the stopping thread its wake-up. Rings are freed here because the
 * producers have quiesced by contract (stop runs after the workers are done)
 * and the generation bump has already invalidated every cached pointer. */
static void writer_teardown(http_log_writer_cb_t *cb)
{
    writer_flush_timer_destroy(cb);

    http_log_sink_t *const sink = cb->sink;

    if (sink != NULL && sink->async_io != NULL) {
        zend_async_io_t *const io = sink->async_io;

        if (io->event.del_callback != NULL) {
            io->event.del_callback(&io->event, &cb->base);
        }

        ZEND_ASYNC_IO_CLOSE(io);

        if (io->event.dispose != NULL) {
            io->event.dispose(&io->event);
        }

        sink->async_io = NULL;
    }

    tsrm_mutex_lock(g_log_lock);
    log_prod_t *r = cb->producers;
    cb->producers = NULL;
    tsrm_mutex_unlock(g_log_lock);

    while (r != NULL) {
        log_prod_t *const next = r->next;
        pefree(r->buf, 1);
        pefree(r, 1);
        r = next;
    }

    zend_async_event_t *const stop_event = cb->stop_event;

    if (sink != NULL) {
        sink->writer_cb = NULL;
    }

    efree(cb);   /* allocated on this thread */

    if (stop_event != NULL) {
        zend_async_trigger_event_t *const trig = (zend_async_trigger_event_t *)stop_event;

        if (trig->trigger != NULL) {
            trig->trigger(trig);   /* cross-thread wake of the stopping thread */
        }
    }
}

/* Log thread: create the io for an already-resolved descriptor, attach the
 * writer and arm the flush timer. The io and the timer must be born on the loop
 * that will drive them, which is why this runs here and not at the call site. */
typedef struct {
    http_log_sink_t      *sink;
    int                   fd;
    http_log_write_mode_t mode;
    bool                  ok;
} log_open_arg_t;

/* A stream socket must be driven as a socket, not as a file. Wrapped as a file,
 * its writes are handed to the blocking IO pool — the same pool that serves
 * sendFile — so a syslog receiver that stops reading parks a pool thread per
 * write until its TCP window is drained. As a socket the write is a poll-driven
 * uv_write on the log thread's loop and back-pressures into the ring instead.
 *
 * Datagram sockets stay on the file path on purpose: a send() to a UDP or
 * unix-datagram peer does not wait on a slow receiver (it drops), so there is
 * nothing to unblock, and libuv's udp handle cannot adopt an already-connected
 * descriptor. Everything else — regular files, stdout, a pipe — keeps its
 * current behaviour. */
static zend_async_io_type log_io_type_for_fd(const int fd)
{
#ifndef PHP_WIN32
    int       type   = 0;
    socklen_t tlen   = sizeof type;

    if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &tlen) != 0
        || type != SOCK_STREAM) {
        return ZEND_ASYNC_IO_TYPE_FILE;   /* not a socket, or a datagram one */
    }

    /* The address family, not SO_DOMAIN — that one is Linux-only. */
    struct sockaddr_storage sa;
    socklen_t               salen = sizeof sa;

    if (getsockname(fd, (struct sockaddr *)&sa, &salen) != 0) {
        return ZEND_ASYNC_IO_TYPE_FILE;
    }

    /* libuv drives an AF_UNIX stream through its pipe handle, not its tcp one. */
    if (sa.ss_family == AF_UNIX) {
        return ZEND_ASYNC_IO_TYPE_PIPE;
    }

    if (sa.ss_family == AF_INET || sa.ss_family == AF_INET6) {
        return ZEND_ASYNC_IO_TYPE_TCP;
    }
#else
    (void)fd;
#endif

    return ZEND_ASYNC_IO_TYPE_FILE;
}

static void log_sink_open_op(void *arg)
{
    log_open_arg_t *const a = (log_open_arg_t *)arg;

    zend_async_io_t *const io =
        ZEND_ASYNC_IO_CREATE((zend_file_descriptor_t)a->fd,
                             log_io_type_for_fd(a->fd),
                             ZEND_ASYNC_IO_WRITABLE | ZEND_ASYNC_IO_PRESERVE_FD);

    if (io == NULL) {
        return;
    }

    http_log_writer_cb_t *const cb = (http_log_writer_cb_t *)
        ZEND_ASYNC_EVENT_CALLBACK_EX(writer_complete_cb, sizeof(http_log_writer_cb_t));

    if (cb == NULL) {
        if (io->event.dispose != NULL) {
            io->event.dispose(&io->event);
        }

        return;
    }

    cb->base.dispose = writer_callback_dispose;
    cb->sink         = a->sink;
    cb->mode         = a->mode;
    cb->producers    = NULL;
    cb->active_req   = NULL;
    cb->active_buf   = NULL;
    cb->stop_event   = NULL;
    ZEND_ATOMIC_BOOL_INIT(&cb->kick_pending, false);
    ZEND_ATOMIC_BOOL_INIT(&cb->closing, false);

    if (UNEXPECTED(!io->event.add_callback(&io->event, &cb->base))) {
        efree(cb);

        if (io->event.dispose != NULL) {
            io->event.dispose(&io->event);
        }

        return;
    }

    a->sink->async_io  = io;
    a->sink->writer_cb = cb;
    writer_flush_timer_create(cb);   /* ticks on this thread's loop */
    a->ok = true;
}

/* Log thread: begin the stop. Marks the sink closing and kicks a final drain;
 * writer_teardown fires the caller's trigger once the rings are empty. */
static void log_sink_close_op(void *arg)
{
    http_log_writer_cb_t *const cb = (http_log_writer_cb_t *)arg;

    zend_atomic_bool_store_ex(&cb->closing, true);
    writer_kick_next(cb);
}

/* Flush timer: fires every HTTP_LOG_FLUSH_INTERVAL_MS for the sink's lifetime;
 * kick_next is a no-op when the ring is empty or a write is in flight. */
static void writer_flush_timer_fn(zend_async_event_t *event,
                                  zend_async_event_callback_t *callback,
                                  void *result, zend_object *exception)
{
    (void)event; (void)result; (void)exception;
    http_log_flush_timer_cb_t *tcb = (http_log_flush_timer_cb_t *)callback;

    if (tcb->writer != NULL) {
        writer_kick_next(tcb->writer);
    }
}

static void writer_flush_timer_dispose(zend_async_event_callback_t *callback,
                                       zend_async_event_t *event)
{
    (void)event;
    efree(callback);
}

/* Create + start the periodic flush timer. Best-effort: on failure the writer
 * still flushes at the high-water mark and after each completion. */
static void writer_flush_timer_create(http_log_writer_cb_t *cb)
{
    zend_async_timer_event_t *t = ZEND_ASYNC_NEW_TIMER_EVENT(
        (zend_ulong)HTTP_LOG_FLUSH_INTERVAL_MS, /*periodic*/ true);

    if (UNEXPECTED(t == NULL)) {
        return;
    }
    ZEND_ASYNC_TIMER_SET_MULTISHOT(t);   /* survive periodic fires */

    http_log_flush_timer_cb_t *tcb = (http_log_flush_timer_cb_t *)
        ZEND_ASYNC_EVENT_CALLBACK_EX(writer_flush_timer_fn, sizeof(*tcb));

    if (UNEXPECTED(tcb == NULL)) {
        t->base.dispose(&t->base);
        return;
    }

    tcb->base.dispose = writer_flush_timer_dispose;
    tcb->writer       = cb;

    if (!t->base.add_callback(&t->base, &tcb->base)) {
        efree(tcb);
        t->base.dispose(&t->base);
        return;
    }

    if (!t->base.start(&t->base)) {
        zend_async_callbacks_remove(&t->base, &tcb->base);
        t->base.dispose(&t->base);
        return;
    }

    cb->flush_timer    = &t->base;
    cb->flush_timer_cb = &tcb->base;
}

/* Stop + dispose the flush timer, nulling the callback's back-pointer first so
 * a tick queued before teardown can't deref the writer being freed. */
static void writer_flush_timer_destroy(http_log_writer_cb_t *cb)
{
    if (cb->flush_timer_cb != NULL) {
        ((http_log_flush_timer_cb_t *)cb->flush_timer_cb)->writer = NULL;

        if (cb->flush_timer != NULL) {
            zend_async_callbacks_remove(cb->flush_timer, cb->flush_timer_cb);
        } else if (cb->flush_timer_cb->dispose != NULL) {
            cb->flush_timer_cb->dispose(cb->flush_timer_cb, NULL);
        }

        cb->flush_timer_cb = NULL;
    }

    if (cb->flush_timer != NULL) {
        if (cb->flush_timer->loop_ref_count > 0) {
            cb->flush_timer->stop(cb->flush_timer);
        }
        cb->flush_timer->dispose(cb->flush_timer);
        cb->flush_timer = NULL;
    }
}


static void log_dispatch_record(http_log_state_t *state,
                                const http_log_record_t *rec);

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
        .category     = HTTP_LOG_CAT_APP,
        .tmpl         = tmpl,
        .body         = body,
        .body_len     = (size_t)n,
        .attrs        = attrs,
        .attrs_count  = attrs_n,
        .has_trace    = false,
    };

    log_dispatch_record(state, &rec);
}


/* Shared fan-out behind emitf and emit_access: format once per distinct
 * (formatter, ud), then hand the bytes to every sink whose floor and
 * category mask admit the record. Past HTTP_LOG_FMT_SLOTS distinct formatters
 * the last slot becomes scratch: it is re-stamped on every miss, so a later
 * sink can never read those bytes under a stale (fn, ud, len). */
static void log_dispatch_record(http_log_state_t *state,
                                const http_log_record_t *rec)
{
    struct { http_log_formatter_fn fn; void *ud; size_t len; } meta[HTTP_LOG_FMT_SLOTS];
    char fbuf[HTTP_LOG_FMT_SLOTS][2048];
    int  slots = 0;

    for (uint8_t i = 0; i < state->sink_count; i++) {
        http_log_sink_t *sink = &state->sinks[i];

        if (sink->severity_floor == HTTP_LOG_OFF
            || (int)rec->severity < (int)sink->severity_floor
            || (sink->category_mask & rec->category) == 0) {
            continue;
        }

        int s = -1;
        for (int k = 0; k < slots; k++) {
            if (meta[k].fn == sink->formatter && meta[k].ud == sink->formatter_ud) {
                s = k;
                break;
            }
        }

        if (s < 0) {
            /* Miss: take a fresh slot, or reuse the last one as scratch once
             * the cache is full. Either way re-stamp its meta, so its bytes
             * and its recorded length always describe the same formatter. */
            s = slots < HTTP_LOG_FMT_SLOTS ? slots++ : HTTP_LOG_FMT_SLOTS - 1;

            meta[s].fn  = sink->formatter;
            meta[s].ud  = sink->formatter_ud;
            meta[s].len = sink->formatter(rec, fbuf[s], sizeof fbuf[s],
                                          sink->formatter_ud);
        }

        if (meta[s].len == 0) {
            continue;
        }

        http_log_sink_write(sink, fbuf[s], meta[s].len);
    }
}

void http_log_emit_access(http_log_state_t *state, const http_access_rec_t *ar)
{
    if (state == NULL || !state->has_access || ar == NULL) {
        return;
    }

    const char *method = ar->method != NULL ? ar->method : "-";
    const char *path   = ar->url_path != NULL ? ar->url_path : "-";

    /* Stable OTel HTTP semconv names (v1.23+). The envelope is OTel Logs, so
     * the attributes have to be too — a collector keys off these exact names. */
    http_log_attr_t attrs[9];
    size_t n = 0;

    attrs[n++] = (http_log_attr_t){ .key = "http.request.method",
                                    .type = HTTP_LOG_ATTR_STR, .v.s = method };
    attrs[n++] = (http_log_attr_t){ .key = "url.path",
                                    .type = HTTP_LOG_ATTR_STR, .v.s = path };

    if (ar->url_query != NULL && ar->url_query[0] != '\0') {
        attrs[n++] = (http_log_attr_t){ .key = "url.query",
                                        .type = HTTP_LOG_ATTR_STR,
                                        .v.s = ar->url_query };
    }

    attrs[n++] = (http_log_attr_t){ .key = "http.response.status_code",
                                    .type = HTTP_LOG_ATTR_I64,
                                    .v.i64 = ar->status };

    if (ar->protocol_version != NULL) {
        attrs[n++] = (http_log_attr_t){ .key = "network.protocol.version",
                                        .type = HTTP_LOG_ATTR_STR,
                                        .v.s = ar->protocol_version };
    }

    attrs[n++] = (http_log_attr_t){ .key = "http.response.body.size",
                                    .type = HTTP_LOG_ATTR_U64,
                                    .v.u64 = ar->response_size };

    /* OTel's unit for http.server.request.duration is seconds, not ms. */
    if (ar->duration_ns != 0) {
        attrs[n++] = (http_log_attr_t){ .key = "http.server.request.duration",
                                        .type = HTTP_LOG_ATTR_F64,
                                        .v.f64 = (double)ar->duration_ns / 1e9 };
    }

    if (ar->client_address != NULL) {
        attrs[n++] = (http_log_attr_t){ .key = "client.address",
                                        .type = HTTP_LOG_ATTR_STR,
                                        .v.s = ar->client_address };
    }

    if (ar->client_port != 0) {
        attrs[n++] = (http_log_attr_t){ .key = "client.port",
                                        .type = HTTP_LOG_ATTR_I64,
                                        .v.i64 = ar->client_port };
    }

    char body[512];
    int  blen = snprintf(body, sizeof body, "%s %s %d", method, path, ar->status);

    if (blen < 0) {
        return;
    }
    if ((size_t)blen >= sizeof body) {
        blen = (int)sizeof body - 1;
    }

    http_log_record_t rec = {
        .state        = state,
        .timestamp_ns = now_realtime_ns(),
        .severity     = HTTP_LOG_INFO,
        .category     = HTTP_LOG_CAT_ACCESS,
        .tmpl         = "access",
        .body         = body,
        .body_len     = (size_t)blen,
        .attrs        = attrs,
        .attrs_count  = n,
        .trace_flags  = ar->trace_flags,
        .has_trace    = ar->trace_id != NULL,
    };

    if (rec.has_trace) {
        memcpy(rec.trace_id, ar->trace_id, sizeof rec.trace_id);
        memcpy(rec.span_id, ar->span_id, sizeof rec.span_id);
    }

    log_dispatch_record(state, &rec);
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

    g_log_lock = tsrm_mutex_alloc();
    ZEND_ATOMIC_INT_INIT(&g_log_generation, 0);

    syslog_hostname_resolve();

    /* Built-ins go through the same registry a plugin extension would use. */
    http_log_register_formatter(&fmt_plain);
    http_log_register_formatter(&fmt_logfmt);
    http_log_register_formatter(&fmt_json);
    http_log_register_formatter(&fmt_pretty);
    /* fmt_syslog is intentionally NOT registered as a public formatter: its
     * output carries no record framing (that is the syslog transport's job), so
     * on a plain stream the records would run together. The syslog sink pins it
     * directly; 'format' => 'syslog' on any other sink is rejected. */
    http_log_register_formatter(&fmt_template);

    http_log_register_sink_type(&sink_type_stream);
    http_log_register_sink_type(&sink_type_file);
    http_log_register_sink_type(&sink_type_stdout);
    http_log_register_sink_type(&sink_type_stderr);
    http_log_register_sink_type(&sink_type_syslog);
}

void http_log_mshutdown(void)
{
    /* Sinks are torn down with their server; only the lock is process-wide. */
    if (g_log_lock != NULL) {
        tsrm_mutex_free(g_log_lock);
        g_log_lock = NULL;
    }
}

/* Recompute the fast gates: `severity` is the lowest floor across app-
 * admitting sinks (the http_logf_* macros' single branch); `has_access` is
 * true when some sink admits INFO access records (the per-request branch). */
static void http_log_state_refresh_gate(http_log_state_t *state)
{
    http_log_severity_t floor      = HTTP_LOG_OFF;
    bool                has_access = false;

    for (uint8_t i = 0; i < state->sink_count; i++) {
        const http_log_sink_t *sink = &state->sinks[i];
        const http_log_severity_t f = sink->severity_floor;

        if (f == HTTP_LOG_OFF) {
            continue;
        }

        if ((sink->category_mask & HTTP_LOG_CAT_APP) != 0
            && (floor == HTTP_LOG_OFF || (int)f < (int)floor)) {
            floor = f;
        }

        if ((sink->category_mask & HTTP_LOG_CAT_ACCESS) != 0
            && (int)f <= (int)HTTP_LOG_INFO) {
            has_access = true;
        }
    }

    state->severity   = floor;
    state->has_access = has_access;
}

/* Build one sink from its spec. The descriptor is resolved here, on the thread
 * that owns the PHP stream; the io, the writer and the flush timer are created
 * on the log thread, which is the only thread that will ever drive them.
 * Returns false (sink left inactive, floor OFF) on any failure — the caller
 * then owns spec's ud. */
static bool http_log_sink_start(http_log_sink_t *sink,
                                const http_log_sink_spec_t *spec)
{
    memset(sink, 0, sizeof *sink);
    sink->severity_floor = HTTP_LOG_OFF;
    ZVAL_UNDEF(&sink->stream_zv);

    if (spec->level == HTTP_LOG_OFF) {
        return false;
    }

    zval *stream_zv = spec->stream_zv;

    if (stream_zv == NULL || Z_TYPE_P(stream_zv) != IS_RESOURCE) {
        return false;
    }

    php_stream *stream = NULL;
    php_stream_from_zval_no_verify(stream, stream_zv);

    if (stream == NULL) {
        return false;
    }

    /* The stream's own async-IO handle is not usable: it belongs to this
     * thread's loop, and the writes run on the log thread's. Take the raw
     * descriptor instead — PRESERVE_FD keeps it with the stream, which stays
     * referenced by sink->stream_zv for as long as the sink lives. */
    int fd = -1;

    if (php_stream_cast(stream, PHP_STREAM_AS_FD | PHP_STREAM_CAST_INTERNAL,
                        (void *)&fd, 0) != SUCCESS || fd < 0) {
        fprintf(stderr,
                "http_server: log stream has no descriptor; sink disabled\n");
        return false;
    }

    tsrm_mutex_lock(g_log_lock);
    const bool have_thread = log_thread_ref();
    tsrm_mutex_unlock(g_log_lock);

    if (!have_thread) {
        fprintf(stderr, "http_server: could not start the log thread; sink disabled\n");
        return false;
    }

    log_open_arg_t arg = { .sink = sink, .fd = fd, .mode = spec->write_mode, .ok = false };

    if (!reactor_pool_exec(g_log_pool, 0, log_sink_open_op, &arg) || !arg.ok) {
        tsrm_mutex_lock(g_log_lock);
        log_thread_unref();
        tsrm_mutex_unlock(g_log_lock);

        fprintf(stderr, "http_server: failed to open the log sink transport\n");
        return false;
    }

    ZVAL_COPY(&sink->stream_zv, stream_zv);
    sink->stream_set        = true;
    sink->formatter         = spec->formatter != NULL ? spec->formatter
                                                      : http_log_format_plain;
    sink->category_mask     = spec->category_mask != 0 ? spec->category_mask
                                                       : HTTP_LOG_CAT_APP;
    sink->formatter_ud      = spec->formatter_ud;
    sink->formatter_ud_free = spec->formatter_ud_free;
    sink->severity_floor    = spec->level;
    return true;
}

/* Stop one sink: hand it to the log thread, which drains what the producers
 * left and tears the transport down, then wait (bounded) for it to report back.
 * Producers must have quiesced — the caller is the server's stop path. */
static void http_log_sink_stop(http_log_sink_t *sink)
{
    /* The formatter ud is ours regardless of the transport's fate — the
     * formatter only runs inside emit, never from an in-flight write. */
    if (sink->formatter_ud_free != NULL && sink->formatter_ud != NULL) {
        sink->formatter_ud_free(sink->formatter_ud);
    }
    sink->formatter_ud      = NULL;
    sink->formatter_ud_free = NULL;
    sink->severity_floor    = HTTP_LOG_OFF;

    http_log_writer_cb_t *const cb = sink->writer_cb;

    if (cb == NULL) {
        goto drop_stream;
    }

    /* Every cached producer ring for this sink dies with it. */
    zend_atomic_int_store_ex(&g_log_generation,
                             zend_atomic_int_load_ex(&g_log_generation) + 1);

    tsrm_mutex_lock(g_log_lock);
    for (log_prod_t *r = cb->producers; r != NULL; r = r->next) {
        zend_atomic_bool_store_ex(&r->dead, true);
    }
    tsrm_mutex_unlock(g_log_lock);

    zend_coroutine_t *const co = ZEND_ASYNC_CURRENT_COROUTINE;
    zend_async_trigger_event_t *const done =
        co != NULL ? ZEND_ASYNC_NEW_TRIGGER_EVENT() : NULL;

    cb->stop_event = done != NULL ? &done->base : NULL;

    if (!reactor_pool_exec(g_log_pool, 0, log_sink_close_op, cb)) {
        /* The log thread is gone: nothing can drain, and nothing will fire the
         * trigger. Leak the cb rather than free a transport we cannot reach. */
        if (done != NULL) {
            ZEND_ASYNC_EVENT_SET_CLOSED(&done->base);
            done->base.dispose(&done->base);
        }

        sink->writer_cb = NULL;
        sink->async_io  = NULL;
        goto drop_stream;
    }

    if (done != NULL) {
        /* The teardown is async (a write may still be in flight). Wait for the
         * log thread's trigger, capped so a wedged descriptor cannot pin the
         * server's shutdown. */
        zend_async_timer_event_t *const timer =
            ZEND_ASYNC_NEW_TIMER_EVENT((zend_ulong)HTTP_LOG_STOP_DRAIN_BUDGET_MS, false);

        if (ZEND_ASYNC_WAKER_NEW(co) != NULL) {
            zend_async_resume_when(co, &done->base, false,
                                   zend_async_waker_callback_resolve, NULL);

            if (timer != NULL) {
                zend_async_resume_when(co, &timer->base, true,
                                       zend_async_waker_callback_timeout, NULL);
            }

            ZEND_ASYNC_SUSPEND();
            zend_async_waker_clean(co);

            if (EG(exception)) {
                zend_clear_exception();   /* budget elapsed → leak path */
            }
        } else if (timer != NULL) {
            timer->base.dispose(&timer->base);
        }

        /* A trigger holds a loop reference (it is a uv_async): closing it first
         * is what lets the caller's loop actually stop at shutdown. */
        ZEND_ASYNC_EVENT_SET_CLOSED(&done->base);
        done->base.dispose(&done->base);
    }

    sink->writer_cb = NULL;
    sink->async_io  = NULL;

    tsrm_mutex_lock(g_log_lock);
    log_thread_unref();
    tsrm_mutex_unlock(g_log_lock);

drop_stream:
    if (sink->stream_set) {
        zval_ptr_dtor(&sink->stream_zv);
        sink->stream_set = false;
        ZVAL_UNDEF(&sink->stream_zv);
    }
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
    state->has_access = false;

    if (n > HTTP_LOG_MAX_SINKS) {
        n = HTTP_LOG_MAX_SINKS;
    }

    for (int i = 0; i < n; i++) {
        if (http_log_sink_start(&state->sinks[state->sink_count], &specs[i])) {
            state->sink_count++;
        } else if (specs[i].formatter_ud_free != NULL
                   && specs[i].formatter_ud != NULL) {
            specs[i].formatter_ud_free(specs[i].formatter_ud);
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
        .write_mode   = HTTP_LOG_WRITE_STREAM,
    };

    http_log_server_start_sinks(state, &spec, 1);
}

void http_log_server_stop(http_log_state_t *state)
{
    if (state == NULL) {
        return;
    }

    state->severity   = HTTP_LOG_OFF;   /* stop new emits immediately */
    state->has_access = false;

    /* Each sink hands itself to the log thread and waits (bounded) for the
     * final drain — see http_log_sink_stop. */
    for (uint8_t i = 0; i < state->sink_count; i++) {
        http_log_sink_stop(&state->sinks[i]);
    }

    state->sink_count = 0;
}
