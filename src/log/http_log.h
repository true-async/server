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
    /* Originating sink — always non-NULL after http_log_emitf has
     * built the record. Writer dispatches through this. */
    struct http_log_state *state;

    uint64_t              timestamp_ns;     /* wall-clock, ns since UNIX epoch */
    http_log_severity_t   severity;
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

typedef void   (*http_log_writer_fn)(const http_log_record_t *rec, void *ud);
typedef size_t (*http_log_formatter_fn)(const http_log_record_t *rec,
                                        char *buf, size_t buf_len, void *ud);

/* Per-server logger state. Embedded inside http_server_object; long-
 * lived structures (http_connection_t, mp_processor_t,
 * http3_connection_t) cache a pointer to it for one-load access on
 * the emit hot path. Multi-thread multi-server is correct because
 * each conn carries its own server's state pointer.
 *
 * `severity` is the first field by intent: the macro hot-path reads
 * it through the state pointer. The other fields are touched only by
 * http_log.c. */
typedef struct http_log_writer_cb http_log_writer_cb_t;

typedef struct http_log_state {
    http_log_severity_t       severity;
    bool                      stream_set;
    zval                      stream_zv;
    zend_async_io_t          *async_io;
    http_log_writer_cb_t     *writer_cb;
    uint64_t                  dropped_total;
    uint64_t                  last_fallback_sec;
} http_log_state_t;

/* Global OFF-sentinel. Used as the fallback target when a connection
 * outlives its server: dangling state pointers get re-pointed here
 * so emits drop silently instead of UAFing. severity is OFF so the
 * gate short-circuits without touching anything else. */
extern http_log_state_t http_log_state_default;

/* Replaces the process-wide writer/formatter. Used by extensions
 * that route emits into their own pipeline (OTel exporter etc.). */
PHPAPI void http_log_set_writer(http_log_writer_fn fn, void *ud);
PHPAPI void http_log_set_formatter(http_log_formatter_fn fn, void *ud);

/* The level gate is re-checked inside, so a call site that skips
 * the http_logf_* macro is still correct (just slower). */
PHPAPI void http_log_emitf(http_log_state_t *state,
                           http_log_severity_t sev,
                           const http_log_attr_t *attrs, size_t attrs_n,
                           const char *tmpl, ...)
    ZEND_ATTRIBUTE_FORMAT(printf, 5, 6);

/* Default formatter: "TS LEVEL body key=val ...\n". */
size_t http_log_format_plain(const http_log_record_t *rec,
                             char *buf, size_t buf_len, void *ud);

extern zend_class_entry *http_log_severity_ce;

void http_log_state_init(http_log_state_t *state);
void http_log_minit(void);
void http_log_mshutdown(void);

/* `stream_zv` must be an IS_RESOURCE zval wrapping a php_stream;
 * the state takes a refcount on it until stop. Severity == OFF or
 * a NULL/IS_NULL stream leaves the logger inactive. */
void http_log_server_start(http_log_state_t *state,
                           http_log_severity_t severity,
                           zval *stream_zv);

/* Idempotent; safe on a never-started state. */
void http_log_server_stop(http_log_state_t *state);

/* UNEXPECTED-wrapped so the disabled path is one branch and skips
 * body formatting entirely. The state argument is read once into a
 * local — multiple macro arg evaluations are forbidden. */
#define http_logf_error(state, tmpl, ...) \
    do { http_log_state_t *_st = (state); \
         if (UNEXPECTED(_st != NULL && _st->severity != HTTP_LOG_OFF \
                        && (int)HTTP_LOG_ERROR >= (int)_st->severity)) \
             http_log_emitf(_st, HTTP_LOG_ERROR, NULL, 0, (tmpl), ##__VA_ARGS__); \
    } while (0)
#define http_logf_warn(state, tmpl, ...) \
    do { http_log_state_t *_st = (state); \
         if (UNEXPECTED(_st != NULL && _st->severity != HTTP_LOG_OFF \
                        && (int)HTTP_LOG_WARN >= (int)_st->severity)) \
             http_log_emitf(_st, HTTP_LOG_WARN, NULL, 0, (tmpl), ##__VA_ARGS__); \
    } while (0)
#define http_logf_info(state, tmpl, ...) \
    do { http_log_state_t *_st = (state); \
         if (UNEXPECTED(_st != NULL && _st->severity != HTTP_LOG_OFF \
                        && (int)HTTP_LOG_INFO >= (int)_st->severity)) \
             http_log_emitf(_st, HTTP_LOG_INFO, NULL, 0, (tmpl), ##__VA_ARGS__); \
    } while (0)
#define http_logf_debug(state, tmpl, ...) \
    do { http_log_state_t *_st = (state); \
         if (UNEXPECTED(_st != NULL && _st->severity != HTTP_LOG_OFF \
                        && (int)HTTP_LOG_DEBUG >= (int)_st->severity)) \
             http_log_emitf(_st, HTTP_LOG_DEBUG, NULL, 0, (tmpl), ##__VA_ARGS__); \
    } while (0)

#define http_logf_warn_a(state, attrs, n, tmpl, ...) \
    do { http_log_state_t *_st = (state); \
         if (UNEXPECTED(_st != NULL && _st->severity != HTTP_LOG_OFF \
                        && (int)HTTP_LOG_WARN >= (int)_st->severity)) \
             http_log_emitf(_st, HTTP_LOG_WARN, (attrs), (n), (tmpl), ##__VA_ARGS__); \
    } while (0)
#define http_logf_error_a(state, attrs, n, tmpl, ...) \
    do { http_log_state_t *_st = (state); \
         if (UNEXPECTED(_st != NULL && _st->severity != HTTP_LOG_OFF \
                        && (int)HTTP_LOG_ERROR >= (int)_st->severity)) \
             http_log_emitf(_st, HTTP_LOG_ERROR, (attrs), (n), (tmpl), ##__VA_ARGS__); \
    } while (0)
#define http_logf_info_a(state, attrs, n, tmpl, ...) \
    do { http_log_state_t *_st = (state); \
         if (UNEXPECTED(_st != NULL && _st->severity != HTTP_LOG_OFF \
                        && (int)HTTP_LOG_INFO >= (int)_st->severity)) \
             http_log_emitf(_st, HTTP_LOG_INFO, (attrs), (n), (tmpl), ##__VA_ARGS__); \
    } while (0)
#define http_logf_debug_a(state, attrs, n, tmpl, ...) \
    do { http_log_state_t *_st = (state); \
         if (UNEXPECTED(_st != NULL && _st->severity != HTTP_LOG_OFF \
                        && (int)HTTP_LOG_DEBUG >= (int)_st->severity)) \
             http_log_emitf(_st, HTTP_LOG_DEBUG, (attrs), (n), (tmpl), ##__VA_ARGS__); \
    } while (0)

#ifdef __cplusplus
}
#endif

#endif /* HTTP_LOG_H */
