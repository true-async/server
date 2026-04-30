/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | This source file is subject to version 3.01 of the PHP license,      |
  | that is bundled with this package in the file LICENSE, and is        |
  | available through the world-wide-web at the following url:           |
  | https://www.php.net/license/3_01.txt                                 |
  | If you did not receive a copy of the PHP license and are unable to   |
  | obtain it through the world-wide-web, please send a note to          |
  | license@php.net so we can mail you a copy immediately.               |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

#ifndef PHP_HTTP_SERVER_H
#define PHP_HTTP_SERVER_H

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <php.h>
#include <Zend/zend_API.h>
#include <Zend/zend_async_API.h>
#include <main/php_network.h>   /* php_socket_t, SOCK_ERR */

#include <stdbool.h>
#include <stdint.h>

extern zend_module_entry http_server_module_entry;
#define phpext_http_server_ptr &http_server_module_entry

#define PHP_HTTP_SERVER_VERSION "0.1.0-dev"

/*
 * ==========================================================================
 * Forward declarations
 * ==========================================================================
 */

typedef struct http1_parser_t http1_parser_t;
typedef struct _http_server_t http_server_t;
typedef struct _http_server_config_t http_server_config_t;
typedef struct _http_connection_t http_connection_t;
typedef struct _http_listen_event_t http_listen_event_t;
typedef struct _http_protocol_strategy_t http_protocol_strategy_t;

/* Backpressure-owning server object (defined in http_server_class.c as
 * file-static). Connections hold a typed pointer to their owning server
 * and call the two hooks below directly — no callback indirection. */
typedef struct http_server_object http_server_object;

/* Persistent (pemalloc) refcounted snapshot of a locked HttpServerConfig.
 * Populated once by http_server_config_lock(); shared across PHP threads
 * via the Config transfer_obj handler. Opaque to everything outside
 * http_server_config.c. */
typedef struct _http_server_shared_config_t http_server_shared_config_t;

/*
 * ==========================================================================
 * Protocol types — single source of truth for the whole project. Other
 * headers must `#include "php_http_server.h"` rather than redefining.
 * ==========================================================================
 */

typedef enum {
    HTTP_PROTOCOL_HTTP1,
    HTTP_PROTOCOL_HTTP2,
    HTTP_PROTOCOL_HTTP3,
    HTTP_PROTOCOL_WEBSOCKET,
    HTTP_PROTOCOL_SSE,
    HTTP_PROTOCOL_GRPC,
    HTTP_PROTOCOL_UNKNOWN
} http_protocol_type_t;

/*
 * ==========================================================================
 * Listener configuration
 * ==========================================================================
 */

/* Listener types */
typedef enum {
    LISTENER_TYPE_TCP,
    LISTENER_TYPE_UNIX,
    LISTENER_TYPE_UDP_H3      /* UDP datagram listener for HTTP/3 (QUIC) */
} http_listener_type_t;

/* Maximum number of listeners per server */
#define HTTP_SERVER_MAX_LISTENERS 16

/* Listener configuration entry */
typedef struct {
    http_listener_type_t type;
    zend_string *host;  /* IP address for TCP, path for Unix socket */
    int port;           /* Port (0 for Unix sockets) */
    bool tls;           /* TLS enabled */
} http_listener_config_t;

/*
 * ==========================================================================
 * HTTP Server Config Structure
 * ==========================================================================
 */

/*
 * Field order: listener array pointer + size_t first (largest), then
 * TLS path pointers, then write buffer, then 32-bit ints (int/uint32_t),
 * then bool flags clustered at the end. zend_object must remain last per
 * PHP object layout contract.
 */
struct _http_server_config_t {
    /* Listeners configuration */
    http_listener_config_t *listeners;
    size_t listener_count;
    size_t listener_capacity;

    /* TLS paths */
    zend_string *tls_cert_path;
    zend_string *tls_key_path;

    /* Buffer sizes */
    size_t write_buffer_size;   /* Write buffer size (default: 65536) */

    /* Connection settings */
    int backlog;            /* Listen backlog (default: 128); passed to listen(2) as int */
    int max_connections;    /* Max concurrent connections (0 = unlimited) */

    /* Admission reject (overload shedding).
     * Maximum in-flight handler coroutines before new H1 requests get 503
     * and new H2 streams get RST_STREAM(REFUSED_STREAM). 0 = disabled,
     * derived at start() to max_connections*10 when config leaves it 0. */
    size_t max_inflight_requests;

    /* Timeouts in seconds (`_s` suffix mirrors the `_ms` suffix used by
     * the per-connection runtime fields, so the unit is always
     * unambiguous at the call site). 0 = disabled. */
    uint32_t read_timeout_s;       /* Read timeout (default: 30) */
    uint32_t write_timeout_s;      /* Write timeout (default: 30) */
    uint32_t keepalive_timeout_s;  /* Keep-alive timeout (default: 5) */
    uint32_t shutdown_timeout_s;   /* Graceful shutdown timeout (default: 5) */

    /* CoDel (accept-side backpressure) target sojourn in milliseconds.
     * Default 5 (RFC 8289). 0 disables CoDel; hard-cap hysteresis still
     * runs when max_connections > 0. See docs/BACKPRESSURE.md. */
    uint32_t backpressure_target_ms;

    /* Connection-drain knobs. All four in milliseconds. Semantics:
     *   max_connection_age_ms        — proactive per-connection drain
     *                                  (gRPC MAX_CONNECTION_AGE). 0 = off.
     *   max_connection_age_grace_ms  — hard-close grace after drain
     *                                  signal. 0 = infinite (no timer).
     *   drain_spread_ms              — reactive-drain spread window
     *                                  (HAProxy close-spread-time).
     *                                  Default 5000.
     *   drain_cooldown_ms            — minimum gap between reactive
     *                                  drain events. Default 10000. */
    uint32_t max_connection_age_ms;
    uint32_t max_connection_age_grace_ms;
    uint32_t drain_spread_ms;
    uint32_t drain_cooldown_ms;

    /* HTTP/2 streaming response per-stream queue cap.
     * When handler's chunk queue exceeds this, send() suspends
     * the coroutine until drain brings it back under. HTTP/1 chunked
     * path ignores this — the kernel send buffer IS the queue. */
    uint32_t stream_write_buffer_bytes;

    /* Maximum request body size accepted by both H1 parser and H2 session
     * (bytes). Default 10 MiB. At server start the value is mirrored into
     * the global parser pool and read by the H2 DATA-frame guard. */
    size_t max_body_size;

    /* HTTP/3 production knobs (PLAN: NEXT_STEPS.md §5).
     *
     *   http3_idle_timeout_ms        — QUIC max_idle_timeout (RFC 9000
     *                                   §10.1). Default 30000. 0 keeps
     *                                   the default; otherwise the value
     *                                   is honoured directly with no
     *                                   artificial ceiling.
     *   http3_stream_window_bytes    — initial_max_stream_data_{bidi_local,
     *                                   bidi_remote,uni}. Single knob, h2o
     *                                   `http3-input-window-size` style.
     *                                   Default 262144 (256 KiB).
     *   http3_max_concurrent_streams — initial_max_streams_bidi. Default
     *                                   100. nginx `http3_max_concurrent_streams`
     *                                   equivalent.
     *   http3_peer_connection_budget — concurrent QUIC connections per
     *                                   peer IP cap. Default 16.
     *   http3_alt_svc_enabled        — RFC 7838 `Alt-Svc: h3=":<port>"`
     *                                   advertisement on H1/H2 responses
     *                                   when an H3 listener is up. Default
     *                                   true. Set false during phased H3
     *                                   rollout. PHP_HTTP3_DISABLE_ALT_SVC
     *                                   env var still wins (legacy
     *                                   override) when present.
     *
     * `initial_max_data` is derived as window × streams (nginx pattern);
     * no direct setter. Env-var overrides (`PHP_HTTP3_IDLE_TIMEOUT_MS`,
     * `PHP_HTTP3_PEER_BUDGET`, `PHP_HTTP3_BENCH_FC`) keep working as ops
     * escape hatches. */
    uint32_t http3_idle_timeout_ms;
    uint32_t http3_stream_window_bytes;
    uint32_t http3_max_concurrent_streams;
    uint32_t http3_peer_connection_budget;
    bool     http3_alt_svc_enabled;

    /* WebSocket knobs (PLAN_WEBSOCKET.md §5).
     *
     *   ws_max_message_size  — reassembled-message cap. Frames whose
     *                          total length crosses this trigger an
     *                          RFC 6455 §7.4.1 "Message Too Big" close
     *                          and a connection teardown. Default 1 MiB.
     *   ws_max_frame_size    — per-frame cap (defence against
     *                          fragment-flood). Currently informational;
     *                          wslay enforces only message length. Default
     *                          1 MiB.
     *   ws_ping_interval_ms  — server-initiated PING cadence. 0 = no
     *                          automatic ping (peer-driven keepalive
     *                          only). Default 30000.
     *   ws_pong_timeout_ms   — deadline after a server PING for the
     *                          peer's PONG to land before we treat the
     *                          connection as dead and close 1001. 0 = no
     *                          timeout. Default 60000. */
    uint32_t ws_max_message_size;
    uint32_t ws_max_frame_size;
    uint32_t ws_ping_interval_ms;
    uint32_t ws_pong_timeout_ms;

    /* Log + telemetry. log_severity is an http_log_severity_t int value
     * (0/5/9/13/17), set via setLogSeverity(LogSeverity). log_stream is
     * an IS_RESOURCE zval pointing at any
     * php_stream the user supplied; IS_UNDEF if not set. telemetry_enabled
     * gates traceparent/tracestate ingestion (Step 5). */
    int      log_severity;
    zval     log_stream;
    bool     telemetry_enabled;

    /* Frozen persistent snapshot — populated at lock time, shared refcounted
     * across threads. NULL until the owning HttpServer constructs (which calls
     * http_server_config_lock) or the Config is transferred directly via a
     * ThreadChannel (lazy freeze). */
    http_server_shared_config_t *frozen;

    /* Boolean flags (clustered) */
    bool http2_enabled;              /* Enable HTTP/2 */
    bool websocket_enabled;          /* Enable WebSocket (TODO: not yet supported) */
    bool protocol_detection_enabled; /* Auto-detect protocol */
    bool tls_enabled;                /* Enable TLS */
    bool auto_await_body;            /* Automatically await request body */
    bool is_locked;                  /* Locked after server start */

    /* Zend object (must be last for PHP object layout) */
    zend_object std;
};

/* Pointer arithmetic from zend_object* to enclosing http_server_config_t.
 * Inline so call-sites don't pay an indirection — the offset is stable
 * by zend object layout (std is the last field). */
static zend_always_inline http_server_config_t *
http_server_config_from_obj(zend_object *obj) {
    return (http_server_config_t *)((char *)obj
                                    - XtOffsetOf(http_server_config_t, std));
}

/*
 * ==========================================================================
 * Listen Event Structure (extends TrueAsync event)
 * ==========================================================================
 */

struct _http_listen_event_t {
    zend_async_listen_event_t base;  /* TrueAsync listen event */
    http_server_t *server;           /* Back-reference to server */
    size_t listener_index;           /* Index in server's listeners array */
};

/*
 * ==========================================================================
 * HTTP Server Structure
 * ==========================================================================
 */

struct _http_server_t {
    /* Configuration (owned reference) */
    http_server_config_t *config;

    /* Listeners (TrueAsync events) */
    http_listen_event_t *listen_events[HTTP_SERVER_MAX_LISTENERS];
    size_t listen_event_count;

    /* Protocol handlers HashTable (protocol_type_str -> handler entry) */
    HashTable protocol_handlers;

    /* Server state */
    bool is_running;
    bool is_stopping;

    /* TrueAsync scope for server coroutines */
    zend_async_scope_t *server_scope;

    /* Event to block start() until stop() is called */
    zend_async_event_t *wait_event;

    /* Statistics */
    size_t active_connections;
    size_t total_requests;

    /* Zend object (must be last for PHP object layout) */
    zend_object std;
};

/*
 * ==========================================================================
 * Module globals
 * ==========================================================================
 */

ZEND_BEGIN_MODULE_GLOBALS(http_server)
    struct {
        http1_parser_t **parsers;
        size_t              count;
        size_t              capacity;
        size_t              max_body_size;
    } parser_pool;
ZEND_END_MODULE_GLOBALS(http_server)

/* Accessor macro for module globals (works with both ZTS and NTS) */
ZEND_EXTERN_MODULE_GLOBALS(http_server)
#ifdef ZTS
#define HTTP_SERVER_G(v) TSRMG(http_server_globals_id, zend_http_server_globals *, v)
#else
#define HTTP_SERVER_G(v) (http_server_globals.v)
#endif

/* Compiler hints for branch prediction */
#if defined(__GNUC__) && __GNUC__ >= 3
# define EXPECTED(condition)   __builtin_expect(!!(condition), 1)
# define UNEXPECTED(condition) __builtin_expect(!!(condition), 0)
#else
# define EXPECTED(condition)   (condition)
# define UNEXPECTED(condition) (condition)
#endif

/*
 * ==========================================================================
 * Module functions
 * ==========================================================================
 */

PHP_MINIT_FUNCTION(http_server);
PHP_MSHUTDOWN_FUNCTION(http_server);
PHP_RINIT_FUNCTION(http_server);
PHP_RSHUTDOWN_FUNCTION(http_server);
PHP_MINFO_FUNCTION(http_server);

/*
 * ==========================================================================
 * Class registration functions
 * ==========================================================================
 */

void http_request_class_register(void);
void uploaded_file_class_register(void);
void http_server_exceptions_register(void);
void http_server_config_class_register(void);
void http_response_class_register(void);
void http_server_class_register(void);

/*
 * ==========================================================================
 * Internal API
 * ==========================================================================
 */

/* Config API */
void http_server_config_lock(zend_object *obj);
bool http_server_config_is_locked(zend_object *obj);

/* Backpressure hooks — called from http_connection on every request
 * completion (sample) and connection close. Implemented in
 * http_server_class.c where the server_object struct is defined.
 * Safe to call with server == NULL (no-op). */
void http_server_on_request_sample(http_server_object *server,
                                   uint64_t sojourn_ns, uint64_t service_ns);
void http_server_on_connection_close(http_server_object *server);

/* Connection-list registry. http_connection_create / _destroy maintain
 * an intrusive list of live connections on each server so http_server_free
 * can NULL their server back-pointers before freeing self — without it,
 * libuv's shutdown drain fires read callbacks against the freed server
 * and on_connection_close UAFs (caught by ASAN 2026-04-28). Both no-op
 * when server == NULL. */
struct _http_connection_t;   /* fwd from src/core/http_connection.h */
void http_server_register_connection(http_server_object *server,
                                     struct _http_connection_t *conn);
void http_server_unregister_connection(http_server_object *server,
                                       struct _http_connection_t *conn);

/* TLS handshake telemetry hooks. on_tls_io is now inline (see counters
 * slice below); these stay out-of-line because they touch state outside
 * the public counters slice (handshake count + ns sum). All safe with
 * server == NULL. */
void http_server_on_tls_handshake_done(http_server_object *server,
                                       uint64_t duration_ns,
                                       bool resumed);
void http_server_on_tls_handshake_failed(http_server_object *server);

/* Post-handshake kTLS probe. tx / rx are independent — the kernel may
 * offload one direction but not the other. */
void http_server_on_tls_ktls(http_server_object *server,
                             bool tx_active, bool rx_active);

/* Parser-error hook. Called once per RFC-compliant 4xx parse-error
 * response built in http_connection_emit_parse_error. */
void http_server_on_parse_error(http_server_object *server, int status_code);

/* H3 dispatch needs the handler fcall + the server scope but lives in
 * src/http3/ where http_server_object's layout is opaque. Two pinpoint
 * getters are clearer than exposing the whole struct. */
HashTable          *http_server_get_protocol_handlers(http_server_object *server);
zend_async_scope_t *http_server_get_scope            (http_server_object *server);

/* Embedded per-server log_state (PLAN_LOG.md). Long-lived structures
 * (http_connection_t, http3_connection_t, mp_processor_t) cache the
 * result at create time. Returns &http_log_state_default for NULL. */
struct http_log_state *http_server_get_log_state(http_server_object *server);

/* Admission-reject predicate. True when the server is overloaded and
 * the caller should short-circuit the request (H1: 503, H2:
 * RST_STREAM REFUSED_STREAM). Returns false if server is NULL or
 * admission is disabled (max_inflight_requests == 0). Hot-path —
 * memory-load only, no locks, no syscalls. */
bool http_server_should_shed_request(const http_server_object *server);

/* http_server_on_request_shed migrated to inline (counters slice). */

/*
 * Graceful connection-drain hooks. Implemented in http_server_class.c
 * where the server_object struct is visible; called from the HTTP/1
 * response-dispose + HTTP/2 commit paths. Both are safe with
 * server == NULL (return false / no-op).
 */

/* Central predicate. Returns true if the connection must close after
 * its current response / stream commit. Implements the pull-model
 * drain logic end-to-end:
 *   1. Proactive: if max_connection_age configured and the connection
 *      has crossed its stamped drain_not_before_ns, flip drain_pending
 *      and bump connections_drained_proactive_total.
 *   2. Reactive: if server->drain_epoch_current advanced past
 *      conn->drain_epoch_seen, flip drain_pending with per-connection
 *      spread-jitter offset (tightening earlier proactive time if the
 *      reactive candidate is sooner) and bump
 *      connections_drained_reactive_total.
 *   3. Return drain_pending && now >= drain_not_before_ns — honours
 *      the spread window.
 *
 * Called at every response-commit point; must be O(1) and inlined-
 * friendly (plain field reads, one hrtime call). */
bool http_server_should_drain_now(http_server_object *server,
                                  http_connection_t *conn);

/* Generic pull-model drain decision. Same semantics as
 * should_drain_now but takes drain state by value and returns the
 * updated state in a struct, so callers whose flags live in bit-fields
 * (cannot have addresses taken) can write the updated values back
 * field-by-field. HTTP/3 reuses this from its own http3_connection_t. */
typedef struct {
    bool     should_drain;        /* return: caller must drain now */
    bool     drain_pending;       /* updated: write back to caller's field */
    uint64_t drain_not_before_ns; /* updated */
    uint64_t drain_epoch_seen;    /* updated */
} http_server_drain_eval_t;

http_server_drain_eval_t http_server_drain_evaluate(http_server_object *server,
                                                    bool drain_pending,
                                                    uint64_t drain_not_before_ns,
                                                    uint64_t drain_epoch_seen);

/* Fired from inside http_server_pause_listeners when its
 * drain_connections argument is true (CoDel trip / hard-cap
 * transition). Bumps drain_epoch_current modulo the cooldown window;
 * if suppressed by cooldown, increments
 * drain_events_cooldown_blocked_total instead so operators can tune. */
void http_server_trigger_drain(http_server_object *server);

/* Accessor — lets http_connection.c stamp drain_not_before_ns at
 * accept time without seeing the http_server_object layout. Returns 0
 * when proactive age-drain is disabled or server is NULL. */
uint64_t http_server_get_max_connection_age_ns(const http_server_object *server);

/* Pre-rendered Alt-Svc header value (RFC 7838). NULL when no
 * H3 listener is configured or PHP_HTTP3_DISABLE_ALT_SVC is set.
 * Returned string is owned by the server, not refcount-borrowed. */
zend_string *http_server_get_alt_svc_value(const http_server_object *server);

/* Set the Alt-Svc header on the response zval iff the handler
 * has not already set one. Called from H1 dispose + H2 commit; H3
 * itself does not advertise Alt-Svc (the client is already on H3). */
void http_response_set_alt_svc_if_unset(zend_object *obj,
                                        const char *value, size_t valuelen);

/* Per-wire-event drain counter bumps migrated to inline (counters slice). */

/*
 * Streaming response — binary interface (vtable) that protocol
 * strategies install on an HttpResponse object so HttpResponse::send()
 * can route chunks without either side seeing the other's layout.
 *
 * HTTP/2 + HTTP/3 plug in stream-aware impls at dispatch time; HTTP/1
 * could later plug in a chunked-transfer-encoder. response.c stays
 * protocol-agnostic — it just consults these hooks.
 */
typedef enum {
    HTTP_STREAM_APPEND_OK             =  0,  /* enqueued, under threshold */
    HTTP_STREAM_APPEND_BACKPRESSURE   =  1,  /* enqueued, over cap — suspend */
    HTTP_STREAM_APPEND_STREAM_DEAD    = -1   /* peer RST / conn closed */
} http_stream_append_result_t;

typedef struct http_response_stream_ops_t http_response_stream_ops_t;
struct http_response_stream_ops_t {
    /* Append a chunk (caller already bumped its refcount). Returns
     * one of http_stream_append_result_t. The op itself knows the
     * threshold (it lives in the context), so send() doesn't need
     * to see server config. */
    int     (*append_chunk)(void *ctx, zend_string *chunk);

    /* Mark stream ended. After all queued chunks drain, the data
     * provider emits EOF (plus trailer HEADERS frame if set).
     * Idempotent. */
    void    (*mark_ended)(void *ctx);

    /* Lazily-created trigger event the handler awaits on under
     * backpressure. Fired by the drain side when the queue drops
     * below threshold. Returns NULL only on alloc failure — callers
     * must NULL-check. */
    zend_async_event_t *(*get_wait_event)(void *ctx);
};

/* Install the streaming vtable + ctx on a response object. The
 * protocol strategy calls this once at dispatch; send() reads it. */
void  http_response_install_stream_ops(zend_object *response_obj,
                                       const http_response_stream_ops_t *ops,
                                       void *ctx);

/* Public counters sub-struct — every write-mostly hot-path counter the
 * server tracks. Embedded inside http_server_object (which stays opaque)
 * so call sites in other TUs can bump fields directly via the inline
 * helpers below — no function call, no NULL check.
 *
 * Connections cache &server->counters (or &http_server_counters_dummy
 * for unsupervised conns) at create time, so the hot path is one load +
 * one inc/add. Adding a new counter here breaks ABI for dependent TUs
 * (rebuild required) — the tradeoff for inline access on hot paths.
 *
 * NOT for stateful counters with side-effects (CoDel sample aggregator,
 * parse-error switch, request_sample) — those stay behind opaque fns. */
typedef struct {
    /* Streaming response */
    uint64_t streaming_responses_total;
    uint64_t stream_send_calls_total;
    uint64_t stream_bytes_sent_total;
    uint64_t stream_send_backpressure_events_total;

    /* HTTP/2 stream-level. h2_streams_active is a gauge (++/--);
     * h2_ping_rtt_ns is the latest sample (overwrite). */
    uint64_t h2_streams_active;
    uint64_t h2_streams_opened_total;
    uint64_t h2_streams_reset_by_peer_total;
    uint64_t h2_streams_refused_total;
    uint64_t h2_goaway_recv_total;
    uint64_t h2_goaway_sent_total;
    uint64_t h2_data_recv_bytes_total;
    uint64_t h2_data_sent_bytes_total;
    uint64_t h2_ping_rtt_ns;

    /* H1 / H3 wire-event drain bumps */
    uint64_t h1_connection_close_sent_total;
    uint64_t h3_goaway_sent_total;

    /* Admission-reject (overload shedding) */
    uint64_t requests_shed_total;

    /* Request lifetime gauge (++ on dispatch, -- on dispose) */
    size_t   active_requests;

    /* TLS bytes — fed per IO-iter from the TLS state machine */
    uint64_t tls_bytes_plaintext_in_total;
    uint64_t tls_bytes_plaintext_out_total;
    uint64_t tls_bytes_ciphertext_in_total;
    uint64_t tls_bytes_ciphertext_out_total;
} http_server_counters_t;

/* Read-mostly config snapshot. Same embedded-pointer pattern: each conn
 * caches &server->view (or &http_server_view_default) at create time.
 * Fields are populated at server start() — getter calls become a single
 * load with no branches. */
typedef struct {
    uint32_t protocol_mask;              /* HTTP_PROTO_MASK_ALL by default */
    uint32_t write_timeout_s;            /* seconds; 0 = disabled */
    uint32_t stream_write_buffer_bytes;  /* per-stream queue cap */
    uint64_t drain_epoch_current;        /* read on every conn create */
    uint32_t http3_idle_timeout_ms;
    uint32_t http3_stream_window_bytes;
    uint32_t http3_max_concurrent_streams;
    uint32_t http3_peer_connection_budget;
    bool     http3_alt_svc_enabled;
    bool     telemetry_enabled;          /* W3C trace context ingestion */
} http_server_view_t;

/* Process-wide fallback objects. Used when a connection has no owning
 * server (unsupervised conn in tests, or after http_server_free clears
 * the back-pointer). Defined in http_server_class.c. The dummy counters
 * is write-mostly garbage; the default view holds the same defaults
 * a fresh server would expose pre-start(). */
extern http_server_counters_t http_server_counters_dummy;
extern const http_server_view_t http_server_view_default;

/* Pointer into the embedded counters slice. Retained for callers that
 * still reach through the server pointer; new code should use
 * conn->counters / session->counters directly. Both fall back to
 * the dummy / default object when server is NULL. */
http_server_counters_t *http_server_counters(http_server_object *server);
const http_server_view_t *http_server_view(const http_server_object *server);

/* Legacy config getters — thin inlines over http_server_view() so old
 * callers compile without churn. New code should pass conn->view. */
static inline uint32_t http_server_get_protocol_mask(const http_server_object *server) {
    return http_server_view(server)->protocol_mask;
}
static inline uint32_t http_server_get_write_timeout_s(const http_server_object *server) {
    return http_server_view(server)->write_timeout_s;
}
static inline uint32_t http_server_get_stream_write_buffer_bytes(const http_server_object *server) {
    return http_server_view(server)->stream_write_buffer_bytes;
}
static inline uint64_t http_server_get_drain_epoch_current(const http_server_object *server) {
    return http_server_view(server)->drain_epoch_current;
}
static inline uint32_t http_server_get_http3_idle_timeout_ms(const http_server_object *server) {
    return http_server_view(server)->http3_idle_timeout_ms;
}
static inline uint32_t http_server_get_http3_stream_window_bytes(const http_server_object *server) {
    return http_server_view(server)->http3_stream_window_bytes;
}
static inline uint32_t http_server_get_http3_max_concurrent_streams(const http_server_object *server) {
    return http_server_view(server)->http3_max_concurrent_streams;
}
static inline uint32_t http_server_get_http3_peer_connection_budget(const http_server_object *server) {
    return http_server_view(server)->http3_peer_connection_budget;
}
static inline bool http_server_get_http3_alt_svc_enabled(const http_server_object *server) {
    return http_server_view(server)->http3_alt_svc_enabled;
}

/* Hot-path increments, inlined at the call site. The pointer is never
 * NULL: callers cache it from conn->counters (which falls back to the
 * dummy) at create time. */
static zend_always_inline void http_server_on_streaming_response_started(http_server_counters_t *c)
{
    c->streaming_responses_total++;
}
static zend_always_inline void http_server_on_stream_send(http_server_counters_t *c, size_t bytes)
{
    c->stream_send_calls_total++;
    c->stream_bytes_sent_total += bytes;
}
static zend_always_inline void http_server_on_stream_backpressure(http_server_counters_t *c)
{
    c->stream_send_backpressure_events_total++;
}

static zend_always_inline void http_server_on_h2_stream_opened(http_server_counters_t *c)
{
    c->h2_streams_active++;
    c->h2_streams_opened_total++;
}
static zend_always_inline void http_server_on_h2_stream_closed(http_server_counters_t *c)
{
    if (c->h2_streams_active > 0) c->h2_streams_active--;
}
static zend_always_inline void http_server_on_h2_ping_rtt(http_server_counters_t *c, uint64_t rtt_ns)
{
    c->h2_ping_rtt_ns = rtt_ns;
}
static zend_always_inline void http_server_on_h2_stream_reset_by_peer(http_server_counters_t *c)
{
    c->h2_streams_reset_by_peer_total++;
}
static zend_always_inline void http_server_on_h2_goaway_recv(http_server_counters_t *c)
{
    c->h2_goaway_recv_total++;
}
static zend_always_inline void http_server_on_h2_goaway_sent(http_server_counters_t *c)
{
    c->h2_goaway_sent_total++;
}
static zend_always_inline void http_server_on_h2_data_recv(http_server_counters_t *c, size_t bytes)
{
    c->h2_data_recv_bytes_total += bytes;
}
static zend_always_inline void http_server_on_h2_data_sent(http_server_counters_t *c, size_t bytes)
{
    c->h2_data_sent_bytes_total += bytes;
}

static zend_always_inline void http_server_on_h1_connection_close_sent(http_server_counters_t *c)
{
    c->h1_connection_close_sent_total++;
}
static zend_always_inline void http_server_on_h3_goaway_sent(http_server_counters_t *c)
{
    c->h3_goaway_sent_total++;
}

static zend_always_inline void http_server_on_request_dispatch(http_server_counters_t *c)
{
    c->active_requests++;
}
static zend_always_inline void http_server_on_request_dispose(http_server_counters_t *c)
{
    if (c->active_requests > 0) c->active_requests--;
}
static zend_always_inline void http_server_on_request_shed(http_server_counters_t *c, bool is_h2)
{
    c->requests_shed_total++;
    if (is_h2) c->h2_streams_refused_total++;
}

static zend_always_inline void http_server_on_tls_io(http_server_counters_t *c,
        size_t plaintext_in, size_t plaintext_out,
        size_t ciphertext_in, size_t ciphertext_out)
{
    c->tls_bytes_plaintext_in_total   += plaintext_in;
    c->tls_bytes_plaintext_out_total  += plaintext_out;
    c->tls_bytes_ciphertext_in_total  += ciphertext_in;
    c->tls_bytes_ciphertext_out_total += ciphertext_out;
}

/*
 * TODO: http_server_config_from_obj() is currently defined locally
 * in http_server_config.c with a different structure type.
 * Full refactoring needed to use http_server_config_t from this header.
 */

/*
 * TODO: http_server_from_obj() is currently defined locally
 * in http_server_class.c with a different structure type (http_server_object).
 * Full refactoring needed to use http_server_t from this header.
 */

/*
 * ==========================================================================
 * Class entries
 * ==========================================================================
 */

extern zend_class_entry *uploaded_file_ce;

/* Exception class entries */
extern zend_class_entry *http_server_exception_ce;
extern zend_class_entry *http_server_runtime_exception_ce;
extern zend_class_entry *http_server_invalid_argument_exception_ce;
extern zend_class_entry *http_server_connection_exception_ce;
extern zend_class_entry *http_server_protocol_exception_ce;
extern zend_class_entry *http_server_timeout_exception_ce;
extern zend_class_entry *http_exception_ce;

/* Config class entry */
extern zend_class_entry *http_server_config_ce;

/* Response class entry */
extern zend_class_entry *http_response_ce;

/* Server class entry */
extern zend_class_entry *http_server_ce;

/*
 * ==========================================================================
 * Response API
 * ==========================================================================
 */

zend_string *http_response_format(zend_object *obj);
zend_string *http_response_format_streaming_headers(zend_object *obj);
void http_response_set_socket(zend_object *obj, php_socket_t fd);
void http_response_set_protocol_version(zend_object *obj, const char *version);
bool http_response_is_closed(zend_object *obj);

/* HTTP/2 strategy uses these to build frames without going through
 * the HTTP/1 text formatter. Headers HashTable is name → array(zval
 * IS_STRING); body pointer is owned by the response object and is
 * valid until the object is destroyed. */
int            http_response_get_status  (zend_object *obj);
HashTable     *http_response_get_headers (zend_object *obj);
HashTable     *http_response_get_trailers(zend_object *obj);
const char    *http_response_get_body    (zend_object *obj, size_t *len_out);

/* Borrow the body's underlying zend_string. Returns NULL when the body
 * is empty. The string is owned by the response object — addref it if
 * you need to outlive the response. Avoids a full body memcpy on the
 * H3 submit path where the response object lives only for the duration
 * of the submit call but the data_reader walks the bytes asynchronously. */
zend_string   *http_response_get_body_str(zend_object *obj);

/* Response-state helpers used by handler-dispose paths (HTTP/1 in
 * http_connection.c, HTTP/2 in src/http2/http2_strategy.c). */
bool  http_response_is_committed   (zend_object *obj);
void  http_response_set_committed  (zend_object *obj);
bool  http_response_is_streaming   (zend_object *obj);  /* send() activated streaming */
void  http_response_reset_to_error (zend_object *obj, int status_code,
                                    const char *message);

/* HttpResponse class entry — the HTTP/2 strategy object-inits a
 * fresh response per stream. HTTP/1 does the same from
 * http_connection_dispatch_request. */
extern zend_class_entry *http_response_ce;

/* The HTTP/2 commit path is fully internal to src/http2/ — the
 * strategy's dispatch spawns a per-stream handler coroutine whose own
 * dispose finalises the response. No
 * HTTP/2-aware hook leaks out of the module. */

/*
 * ==========================================================================
 * Parser pool API
 * ==========================================================================
 */

http1_parser_t* parser_pool_acquire(void);
void parser_pool_return(http1_parser_t *ctx);
void parser_pool_clear(void);

#endif	/* PHP_HTTP_SERVER_H */
