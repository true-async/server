/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

#ifndef HTTP_LOG_H
#define HTTP_LOG_H

#include "php.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* OTel SeverityNumber values. TRACE is omitted (unused); FATAL is
 * absent because zend_error_noreturn(E_ERROR) handles termination. */
typedef enum {
    HTTP_LOG_OFF   = 0,
    HTTP_LOG_DEBUG = 5,
    HTTP_LOG_INFO  = 9,
    HTTP_LOG_WARN  = 13,
    HTTP_LOG_ERROR = 17,
} http_log_severity_t;

typedef enum {
    HTTP_LOG_ATTR_STR,
    HTTP_LOG_ATTR_I64,
    HTTP_LOG_ATTR_U64,
    HTTP_LOG_ATTR_BOOL,
    HTTP_LOG_ATTR_F64,
} http_log_attr_type_t;

/* Record categories: 'app' = server diagnostics (every http_logf_* macro),
 * 'access' = the one-per-request access record. A sink admits a record when
 * its category mask carries the record's bit. The spec key 'category' maps
 * app (DEFAULT — so enabling diagnostics never silently turns on per-request
 * logging) | access | all. */
typedef enum {
    HTTP_LOG_CAT_APP    = 1u << 0,
    HTTP_LOG_CAT_ACCESS = 1u << 1,
} http_log_category_t;

typedef struct {
    const char           *key;
    http_log_attr_type_t  type;
    union {
        const char *s;
        int64_t     i64;
        uint64_t    u64;
        bool        b;
        double      f64;
    } v;
} http_log_attr_t;

/* `tmpl` is the unsubstituted format string and doubles as a stable
 * event identity (Serilog message-template style). Named `tmpl`
 * because `template` is a reserved keyword in C++ and the header must
 * stay includable from C++ TUs. */
typedef struct http_log_state http_log_state_t_fwd;
typedef struct {
    /* Originating logger state — always non-NULL after http_log_emitf
     * has built the record. Carries per-server context (e.g. worker_id)
     * for formatters; the fan-out picks the sink, not this field. */
    struct http_log_state *state;

    uint64_t              timestamp_ns;     /* wall-clock, ns since UNIX epoch */
    http_log_severity_t   severity;
    uint8_t               category;         /* http_log_category_t bit */
    const char           *tmpl;
    const char           *body;
    size_t                body_len;
    const http_log_attr_t *attrs;
    size_t                 attrs_count;

    uint8_t  trace_id[16];
    uint8_t  span_id[8];
    uint8_t  trace_flags;
    bool     has_trace;
} http_log_record_t;

typedef size_t (*http_log_formatter_fn)(const http_log_record_t *rec,
                                        char *buf, size_t buf_len, void *ud);

typedef struct http_log_writer_cb http_log_writer_cb_t;

/* How the sink's writer turns formatted records into transport writes.
 * Record framing is a TRANSPORT property, not a formatter one — the same
 * RFC 5424 message is octet-framed on TCP but must be one datagram on UDP. */
typedef enum {
    /* Byte stream: records merge, one write drains everything buffered. */
    HTTP_LOG_WRITE_STREAM = 0,
    /* Byte stream with RFC 6587 octet-count framing ("LEN SP MSG") applied
     * per record, so a receiver splits records even with embedded LFs. */
    HTTP_LOG_WRITE_STREAM_FRAMED,
    /* Message boundaries preserved: exactly one record per write, so each
     * record travels as one datagram (UDP / unix-dgram syslog). */
    HTTP_LOG_WRITE_DGRAM,
} http_log_write_mode_t;

/* One logging destination: a severity floor, a formatter, and an async
 * transport. A record fans out to every sink whose floor admits it, each
 * rendering with its own formatter. Sinks are independent — one failing
 * (drop-counted, rate-limited stderr notice) never blocks the others. */
typedef struct http_log_sink {
    http_log_severity_t       severity_floor;
    uint8_t                   category_mask;   /* http_log_category_t bits */
    http_log_formatter_fn     formatter;
    void                     *formatter_ud;
    void                    (*formatter_ud_free)(void *ud);

    /* Async file-writer transport: one write in flight, later emits
     * coalesce into pending (writer_cb). Owns its own drop counter and
     * stderr-fallback rate-limit so a drop is attributed to this sink. */
    bool                      stream_set;
    zval                      stream_zv;
    zend_async_io_t          *async_io;
    http_log_writer_cb_t     *writer_cb;
    uint64_t                  dropped_total;
    uint64_t                  last_fallback_sec;
} http_log_sink_t;

#define HTTP_LOG_MAX_SINKS 8

/* Per-server logger state. Embedded inside http_server_object; long-
 * lived structures (http_connection_t, mp_processor_t,
 * http3_connection_t) cache a pointer to it for one-load access on
 * the emit hot path. Multi-thread multi-server is correct because
 * each conn carries its own server's state pointer.
 *
 * `severity` is the first field by intent: the macro hot-path reads it
 * through the state pointer. It holds the minimum floor across all sinks,
 * so a level below every sink short-circuits in one branch. The sinks
 * themselves are touched only by http_log.c. */
typedef struct http_log_state {
    /* Minimum floor across app-admitting sinks — the http_logf_* gate. */
    http_log_severity_t       severity;
    /* One-branch gate for the per-request access emit: true when some sink
     * admits ACCESS records at INFO. */
    bool                      has_access;
    uint8_t                   sink_count;
    http_log_sink_t           sinks[HTTP_LOG_MAX_SINKS];
} http_log_state_t;

/* Global OFF-sentinel. Used as the fallback target when a connection
 * outlives its server: dangling state pointers get re-pointed here
 * so emits drop silently instead of UAFing. severity is OFF so the
 * gate short-circuits without touching anything else. */
extern http_log_state_t http_log_state_default;

/* One completed request, as the logger sees it — a POD so this layer needs no
 * request, response or protocol types. Fields carry OTel HTTP semconv names.
 * Pointers are borrowed for the emit; NULL/0 omits the attribute. */
typedef struct {
    const char *method;           /* http.request.method */
    const char *url_path;         /* url.path */
    const char *url_query;        /* url.query */
    const char *protocol_version; /* network.protocol.version: "1.1"|"2"|"3" */
    int         status;           /* http.response.status_code */
    uint64_t    response_size;    /* http.response.body.size */
    uint64_t    duration_ns;      /* http.server.request.duration (emitted in s) */
    const char *client_address;   /* client.address — bare IP */
    uint16_t    client_port;      /* client.port */
    const uint8_t *trace_id;      /* 16 bytes; NULL = no trace context */
    const uint8_t *span_id;       /* 8 bytes */
    uint8_t        trace_flags;
} http_access_rec_t;

/* Severity INFO. Gated on state->has_access; call sites wrap in UNEXPECTED(). */
void http_log_emit_access(http_log_state_t *state,
                          const http_access_rec_t *rec);

/* The level gate is re-checked inside, so a call site that skips
 * the http_logf_* macro is still correct (just slower). */
void http_log_emitf(http_log_state_t *state,
                    http_log_severity_t sev,
                    const http_log_attr_t *attrs, size_t attrs_n,
                    const char *tmpl, ...)
    ZEND_ATTRIBUTE_FORMAT(printf, 5, 6);

/* Built-in formatters. plain: "TS LEVEL body key=val ...\n" (default).
 * logfmt: "ts=… level=… msg=… key=value …\n". json: one OTel-Logs object
 * per line. pretty: "HH:MM:SS.mmm  LEVEL  body key=val …\n" for consoles —
 * colour is decided once at sink build and passed via the formatter's `ud`
 * (non-NULL = colour on). All four share one attribute-iteration helper. */
size_t http_log_format_plain(const http_log_record_t *rec,
                             char *buf, size_t buf_len, void *ud);
size_t http_log_format_logfmt(const http_log_record_t *rec,
                              char *buf, size_t buf_len, void *ud);
size_t http_log_format_json(const http_log_record_t *rec,
                            char *buf, size_t buf_len, void *ud);
size_t http_log_format_pretty(const http_log_record_t *rec,
                              char *buf, size_t buf_len, void *ud);

/* User-template line: `ud` is a compiled template from
 * http_log_template_parse. Placeholders: {ts} (ISO-8601) / {ts:PATTERN}
 * (PHP date()-style subset: Y y m d H i s v), {level}, {msg}, {attrs},
 * {trace}, {span}; anything else is literal. NULL ud falls back to plain. */
size_t http_log_format_template(const http_log_record_t *rec,
                                char *buf, size_t buf_len, void *ud);

#define HTTP_LOG_TEMPLATE_MAX 256

/* Compile a template into the segment list http_log_format_template renders.
 * Returns an efree()-able block, or NULL when the template is empty, too
 * long, or has too many segments. */
void *http_log_template_parse(const char *tmpl, size_t len);

/* Bare RFC 5424 syslog message ("<PRI>1 TS HOST APP PROCID - - MSG"); the
 * transport frames it (octet-count on a stream, one datagram on UDP/unix).
 * `ud` carries the syslog facility (0..23, default user=1). */
size_t http_log_format_syslog(const http_log_record_t *rec,
                              char *buf, size_t buf_len, void *ud);

/* RFC 5424 facility keyword (e.g. "user", "daemon", "local0") → code, or -1. */
int http_log_syslog_facility(const char *name, size_t len);

/* 'category' spec keyword → category bits: app | access | all. 0 = unknown. */
uint8_t http_log_category_mask(const char *name, size_t len);

/*
 * Sink-type / formatter registry — the plugin seam. Built-ins register here
 * at MINIT (http_log_minit); another extension adds its own by calling the
 * register functions from its MINIT (after this module's). Registration is
 * MINIT-only and therefore single-threaded; the def structs must be static
 * (the registry stores the pointers, not copies).
 */

typedef struct {
    const char            *name;
    http_log_formatter_fn  fn;
    /* Config-time validation of formatter-specific spec keys (e.g. template's
     * 'template' string); throws and returns false. NULL = none. */
    bool (*validate)(HashTable *spec);
    /* Resolve per-sink formatter state from the (validated) spec and the
     * resolved transport stream — e.g. pretty's colour flag, syslog's
     * facility, template's compiled segments. NULL when the formatter
     * carries no state. */
    void *(*make_ud)(HashTable *spec, zval *stream_zv);
    /* Release a make_ud result at sink stop. NULL when ud is not owned
     * (flag/scalar uds). Called only on a non-NULL ud. */
    void (*free_ud)(void *ud);
} http_log_formatter_def_t;

typedef struct {
    const char *name;
    /* Config-time validation beyond the common keys (type/format/level);
     * throws and returns false on violation. NULL = nothing extra. */
    bool (*validate)(HashTable *spec);
    /* Resolve the sink's transport into stream_out as an owned zval ref
     * (released by the caller after the sink takes its own) and set the
     * writer mode (pre-initialised to STREAM — only touch it for framed or
     * datagram transports). false = skip this sink (e.g. target
     * unreachable). Runs at server start. */
    bool (*open)(HashTable *spec, zval *stream_out, http_log_write_mode_t *mode);
    /* Non-NULL forces this formatter (the spec's 'format' is ignored) —
     * how syslog pins its wire format. NULL → the spec's 'format' picks. */
    const http_log_formatter_def_t *pinned_formatter;
} http_log_sink_type_t;

/* No sink delivers to a PHP callback by design: emits fire from IO-completion
 * callbacks, teardown, and H3 reactor threads, which have no TSRM context.
 * Userland exporters drain a file/socket sink from their own coroutine. */

/* false when the registry is full or the name is already taken. */
bool http_log_register_formatter(const http_log_formatter_def_t *def);
bool http_log_register_sink_type(const http_log_sink_type_t *type);

const http_log_formatter_def_t *http_log_formatter_by_name(const char *name,
                                                           size_t len);
const http_log_sink_type_t *http_log_sink_type_by_name(const char *name,
                                                       size_t len);

/* "a|b|c" join of registered names, for error messages. */
void http_log_sink_type_names(char *buf, size_t cap);
void http_log_formatter_names(char *buf, size_t cap);

/* Resolve whether a pretty sink writing to `fd` should colour: NO_COLOR off,
 * else CLICOLOR_FORCE on, else follows isatty(fd). Called once at sink build. */
bool http_log_color_for_fd(int fd);

extern zend_class_entry *http_log_severity_ce;

/* Where this thread's dropped records are counted. Each producer thread points
 * this at its own counters slice, so the bump is race-free and getStats can
 * attribute the loss. NULL (the default) just skips the count. */
void http_log_set_thread_drop_counter(uint64_t *counter);

void http_log_state_init(http_log_state_t *state);
void http_log_minit(void);
void http_log_mshutdown(void);

/* One sink to build at server start. `stream_zv` is an IS_RESOURCE zval
 * wrapping a php_stream (the state takes a refcount until stop); `formatter`
 * NULL defaults to plain; `formatter_ud` feeds the formatter (pretty colour
 * flag). A spec whose level is OFF or whose stream is unusable is skipped. */
typedef struct {
    http_log_severity_t    level;
    uint8_t                category_mask;   /* 0 → HTTP_LOG_CAT_APP */
    http_log_formatter_fn  formatter;
    void                  *formatter_ud;
    void                 (*formatter_ud_free)(void *ud);   /* owned ud, or NULL */
    zval                  *stream_zv;
    http_log_write_mode_t  write_mode;
} http_log_sink_spec_t;

/* Activate the logger with an explicit list of sinks (cap HTTP_LOG_MAX_SINKS;
 * extra specs are ignored). Replaces any prior config. */
void http_log_server_start_sinks(http_log_state_t *state,
                                  const http_log_sink_spec_t *specs, int n);

/* Single-stream sugar: one plain sink from severity + stream_zv. Severity ==
 * OFF or a NULL/IS_NULL stream leaves the logger inactive. */
void http_log_server_start(http_log_state_t *state,
                           http_log_severity_t severity,
                           zval *stream_zv);

/* Idempotent; safe on a never-started state. */
void http_log_server_stop(http_log_state_t *state);

/* Single gate behind every level macro: UNEXPECTED-wrapped so the disabled
 * path is one branch and skips body formatting entirely. The state argument
 * is read once into a local — multiple macro arg evaluations are forbidden. */
#define http_logf_at(state, lvl, attrs, n, tmpl, ...) \
    do { http_log_state_t *_st = (state); \
         if (UNEXPECTED(_st != NULL && _st->severity != HTTP_LOG_OFF \
                        && (int)(lvl) >= (int)_st->severity)) \
             http_log_emitf(_st, (lvl), (attrs), (n), (tmpl), ##__VA_ARGS__); \
    } while (0)

#define http_logf_error(state, tmpl, ...) \
    http_logf_at((state), HTTP_LOG_ERROR, NULL, 0, (tmpl), ##__VA_ARGS__)
#define http_logf_warn(state, tmpl, ...) \
    http_logf_at((state), HTTP_LOG_WARN, NULL, 0, (tmpl), ##__VA_ARGS__)
#define http_logf_info(state, tmpl, ...) \
    http_logf_at((state), HTTP_LOG_INFO, NULL, 0, (tmpl), ##__VA_ARGS__)
#define http_logf_debug(state, tmpl, ...) \
    http_logf_at((state), HTTP_LOG_DEBUG, NULL, 0, (tmpl), ##__VA_ARGS__)

#define http_logf_error_a(state, attrs, n, tmpl, ...) \
    http_logf_at((state), HTTP_LOG_ERROR, (attrs), (n), (tmpl), ##__VA_ARGS__)
#define http_logf_warn_a(state, attrs, n, tmpl, ...) \
    http_logf_at((state), HTTP_LOG_WARN, (attrs), (n), (tmpl), ##__VA_ARGS__)
#define http_logf_info_a(state, attrs, n, tmpl, ...) \
    http_logf_at((state), HTTP_LOG_INFO, (attrs), (n), (tmpl), ##__VA_ARGS__)
#define http_logf_debug_a(state, attrs, n, tmpl, ...) \
    http_logf_at((state), HTTP_LOG_DEBUG, (attrs), (n), (tmpl), ##__VA_ARGS__)

#ifdef __cplusplus
}
#endif

#endif /* HTTP_LOG_H */
