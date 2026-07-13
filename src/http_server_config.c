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
#include "zend_exceptions.h"
#include "ext/json/php_json.h"             /* PHP_JSON_* flags */
#include "Zend/zend_atomic.h"
#include "Zend/zend_async_API.h"
#include "Zend/zend_virtual_cwd.h"        /* VCWD_STAT, VCWD_ACCESS — crossplat */
#include "Zend/zend_enum.h"
#include "Zend/zend_closures.h"           /* zend_ce_closure */
#include "php_streams.h"                  /* php_stream_from_zval_no_verify */
#include "php_http_server.h"
#include "core/http_protocol_strategy.h"  /* HTTP_PROTO_MASK_* */
#include "log/http_log.h"                 /* http_log_severity_ce */
#ifdef HAVE_HTTP_COMPRESSION
#include "compression/http_compression_defaults.h"
#endif

#include <stdint.h>
#include <sys/stat.h>

/* Include generated arginfo */
#include "../stubs/HttpServerConfig.php_arginfo.h"

/* Persistent snapshot of a locked config. Shared (refcounted) across PHP
 * threads that receive the Config through transfer_obj. Listener hosts and
 * TLS paths are persistent zend_strings (pemalloc); the listener array is
 * pemalloc'd as well.
 *
 * Declared here (private) — public API only hands out a forward-declared
 * opaque pointer via php_http_server.h. */
typedef struct {
    http_listener_type_t type;
    zend_string         *host;   /* persistent zend_string */
    int                  port;
    bool                 tls;
    uint32_t             protocol_mask;  /* HTTP_PROTO_MASK_* per listener */
} http_listener_shared_t;

struct _http_server_shared_config_t {
    zend_atomic_int         ref_count;

    http_listener_shared_t *listeners;           /* pemalloc array */
    size_t                  listener_count;

    zend_string            *tls_cert_path;       /* persistent zend_string or NULL */
    zend_string            *tls_key_path;
    zend_string            *http3_hq_docroot;    /* persistent; hq-interop docroot */

    size_t                  write_buffer_size;
    int                     backlog;
    int                     max_connections;
    int                     workers;                /* built-in pool size; 1 = off (issue #11) */
    size_t                  max_inflight_requests;  /* 0 = disabled */
    uint32_t                read_timeout_s;
    uint32_t                write_timeout_s;
    uint32_t                keepalive_timeout_s;
    uint32_t                shutdown_timeout_s;
    uint32_t                backpressure_target_ms;

    /* Drain knobs */
    uint32_t                max_connection_age_ms;
    uint32_t                max_connection_age_grace_ms;
    uint32_t                drain_spread_ms;
    uint32_t                drain_cooldown_ms;
    uint32_t                stream_write_buffer_bytes;
    size_t                  max_body_size;
    size_t                  h2_static_budget_max;

    /* H3 production knobs — see header for semantics. */
    uint32_t                http3_idle_timeout_ms;
    uint32_t                http3_stream_window_bytes;
    uint32_t                http3_max_concurrent_streams;
    uint32_t                http3_peer_connection_budget;
    uint32_t                http3_socket_buffer_bytes;
    uint32_t                tls_buffer_bytes;

    /* per-reactor inbound command-mailbox depth; 0 = engine default (#106) */
    uint32_t                reactor_mailbox_capacity;

    /* WS knobs — see http_server_config_t for semantics. */
    uint32_t                ws_max_message_size;
    uint32_t                ws_max_frame_size;
    uint32_t                ws_ping_interval_ms;
    uint32_t                ws_pong_timeout_ms;
    bool                    ws_permessage_deflate;

    /* Compression — see http_server_config_t for semantics. The MIME
     * whitelist is deep-copied to a persistent zend_string array so
     * cross-thread LOAD can rebuild it without touching the source
     * thread's HashTable. */
    bool                    compression_enabled;
    uint8_t                 compression_level;
    uint8_t                 brotli_level;
    uint8_t                 zstd_level;
    size_t                  compression_min_size;
    size_t                  request_max_decompressed_size;
    zend_string           **compression_mime_types;     /* persistent strings */
    size_t                  compression_mime_count;
    uint32_t                json_encode_flags;

    bool                    http2_enabled;
    bool                    websocket_enabled;
    bool                    protocol_detection_enabled;
    bool                    tls_enabled;
    bool                    auto_await_body;
    bool                    body_streaming_enabled;   /* Issue #26 */
    /* Alt-Svc: h3=":<port>"; ma=86400 emission on H1/H2 responses when an
     * H3 listener is up. Default true. setHttp3AltSvcEnabled(false) lets
     * operators roll out H3 without clients migrating yet — replaces the
     * legacy PHP_HTTP3_DISABLE_ALT_SVC env var (which doesn't propagate
     * reliably to child PHP processes on Windows). The env var is still
     * honoured at start() time as an escape hatch. */
    bool                    http3_alt_svc_enabled;
    bool                    http3_pacing;
    bool                    request_scope;
    bool                    stats_enabled;   /* issue #5: opt-in getStats() */

    /* Logging (issue #5): sink specs flattened to persistent strings so each
     * worker LOAD rebuilds its own sinks (file/stdout/stderr/syslog reopen
     * their transport per worker). 'stream' (a parent-thread resource) and
     * 'php' (a closure) cannot cross threads — skipped at freeze with a
     * stderr notice. Absent keys are NULL. */
    struct http_shared_log_sink {
        zend_string *type;
        zend_string *format;
        zend_string *target;
        zend_string *facility;
        zend_string *path;
        zend_string *tmpl;
        zend_string *category;
        int          level;
    }                      *log_sinks;
    size_t                  log_sink_count;
};

/* Forward declarations for shared-config lifecycle helpers */
static http_server_shared_config_t *http_server_shared_config_freeze(const http_server_config_t *src);
static void http_server_shared_config_addref(http_server_shared_config_t *shared);
static void http_server_shared_config_release(http_server_shared_config_t *shared);
static void http_server_config_populate_from_shared(
    http_server_config_t *dst, const http_server_shared_config_t *src);

#define DEFAULT_BACKLOG                 128
#define DEFAULT_MAX_CONNECTIONS         0      /* Unlimited */
#define DEFAULT_READ_TIMEOUT            30     /* 30 seconds */
#define DEFAULT_WRITE_TIMEOUT           30     /* 30 seconds */
#define DEFAULT_KEEPALIVE_TIMEOUT       5      /* 5 seconds */
#define DEFAULT_SHUTDOWN_TIMEOUT        5      /* 5 seconds */
#define DEFAULT_WRITE_BUFFER_SIZE       65536  /* 64KB */
#define DEFAULT_BACKPRESSURE_TARGET_MS  0      /* CoDel off by default — sojourn-based AQM
                                                * misfires on HTTP/2 mux (sustained pipeline
                                                * looks like persistent queue). Opt-in via
                                                * setBackpressureTargetMs() / CODEL_TARGET_MS. */

/* Drain defaults.
 * Age + grace default 0 = "proactive drain off"; opt-in by operator.
 * Spread + cooldown have working defaults — they only ever take effect
 * if a reactive trigger fires (CoDel-trip or hard-cap transition). */
#define DEFAULT_MAX_CONNECTION_AGE_MS       0      /* off (opt-in, gRPC convention) */
#define DEFAULT_MAX_CONN_AGE_GRACE_MS       0      /* infinite grace (no force-close) */
#define DEFAULT_DRAIN_SPREAD_MS             5000   /* HAProxy close-spread-time style */
#define DEFAULT_DRAIN_COOLDOWN_MS           10000  /* prevent drain oscillation */
#define DEFAULT_STREAM_WRITE_BUFFER_BYTES   262144 /* 256 KiB — gRPC middle */
#define STREAM_WRITE_BUFFER_MIN_BYTES       4096
/* 4 MiB ceiling: even at MAX_CONCURRENT_STREAMS (100) the worst-case
 * per-connection staging memory stays bounded (~400 MiB), not the
 * 6.4 GiB a 64 MiB cap would have allowed. */
#define STREAM_WRITE_BUFFER_MAX_BYTES       (4u * 1024u * 1024u)

/* max_body_size applies to both H1 parser and H2 session. Upper bound
 * set at 16 GiB: realistic for uploads, stays well under SIZE_MAX/2 on
 * 32-bit builds, and keeps smart_str arithmetic in cb_on_data_chunk_recv
 * comfortably overflow-safe. */
#define DEFAULT_MAX_BODY_SIZE               (10u * 1024u * 1024u)          /* 10 MiB */
#define MAX_BODY_SIZE_MIN                   1024u                          /* 1 KiB */
#define MAX_BODY_SIZE_MAX                   ((size_t)16 * 1024 * 1024 * 1024) /* 16 GiB */

/* WebSocket defaults. */
#define DEFAULT_WS_MAX_MESSAGE_SIZE     (1u * 1024u * 1024u)   /* 1 MiB */
#define DEFAULT_WS_MAX_FRAME_SIZE       (1u * 1024u * 1024u)   /* 1 MiB */
#define DEFAULT_WS_PING_INTERVAL_MS     30000u
#define DEFAULT_WS_PONG_TIMEOUT_MS      60000u
#define WS_MAX_MESSAGE_SIZE_MIN         128u
#define WS_MAX_MESSAGE_SIZE_MAX         (256u * 1024u * 1024u) /* 256 MiB hard cap */

/* H3 defaults (NEXT_STEPS.md §5). Values mirror the in-tree QUIC
 * `transport_params` defaults at http3_connection.c:2200-2230. Bounds
 * are deliberately wide — RFC 9000 has no hard ceiling on most of these,
 * and a too-narrow validator would re-introduce the very ceilings we
 * are removing. Sanity bounds only: positive, fits machine word. */
#define DEFAULT_HTTP3_IDLE_TIMEOUT_MS         30000u    /* 30 s */
#define DEFAULT_HTTP3_STREAM_WINDOW_BYTES     (256u * 1024u)
#define HTTP3_STREAM_WINDOW_MIN_BYTES         1024u
#define HTTP3_STREAM_WINDOW_MAX_BYTES         (1024u * 1024u * 1024u)  /* 1 GiB */
#define DEFAULT_HTTP3_MAX_CONCURRENT_STREAMS  100u
#define HTTP3_MAX_CONCURRENT_STREAMS_MAX      1000000u
/* 0 = opt-in / disabled by default. Neither nginx nor h2o ships a
 * default per-source-IP connection cap; a low one collapses legitimate
 * shared-IP fan-out (CGNAT, proxies, a loopback load generator). Source-
 * address validation (Retry) is the amplification defence and the global
 * max_connections gate is the resource backstop. Operators who want a
 * per-IP throttle set it explicitly via setHttp3PeerConnectionBudget(). */
#define DEFAULT_HTTP3_PEER_BUDGET             0u
#define HTTP3_PEER_BUDGET_MAX                 4096u
#define REACTOR_MAILBOX_CAP_MIN               64u                    /* = reactor drain batch */
#define REACTOR_MAILBOX_CAP_MAX               (1u << 20)             /* 1,048,576 */
#define DEFAULT_HTTP3_SOCKET_BUFFER_BYTES     (8u * 1024u * 1024u)   /* 8 MiB */
#define HTTP3_SOCKET_BUFFER_MAX               (256u * 1024u * 1024u) /* 256 MiB */

/* TLS CT-out BIO ring (#29). One TLS record's ciphertext is ~17 KiB
 * (16 KiB payload + AEAD/header overhead) — TLS_BIO_RING_SIZE_SMALL in
 * tls_layer.h. The setter rounds the requested bytes UP to a whole number
 * of records so the ring never holds a partial record (which would park
 * needlessly), floored at one record (below that the handshake breaks).
 * Default matches TLS_BIO_RING_SIZE (64 KiB). 0 => default. */
#define TLS_BUFFER_RECORD_BYTES               (17u * 1024u)          /* 1 record ciphertext */
#define DEFAULT_TLS_BUFFER_BYTES              (64u * 1024u)          /* = TLS_BIO_RING_SIZE */
#define TLS_BUFFER_MAX_RECORDS                16u
#define TLS_BUFFER_MAX                        (TLS_BUFFER_RECORD_BYTES * TLS_BUFFER_MAX_RECORDS)

#ifdef HAVE_HTTP_COMPRESSION
/* Compression knob defaults are sourced from
 * include/compression/http_compression_defaults.h so policy and the
 * HttpServerConfig setters share a single source of truth. */

/* Strip MIME parameters (`; charset=utf-8`), trim, lowercase. Returns
 * an emalloc'd zend_string. NULL on empty/blank input — callers reject
 * such entries. Done once at setter time so the per-request match path
 * does no normalisation work. */
static zend_string *http_compression_normalize_mime(const char *src, size_t len)
{
    while (len > 0 && (src[0] == ' ' || src[0] == '\t')) { src++; len--; }
    size_t end = len;
    for (size_t i = 0; i < len; i++) {
        if (src[i] == ';') { end = i; break; }
    }
    while (end > 0 && (src[end - 1] == ' ' || src[end - 1] == '\t')) end--;

    if (end == 0) return NULL;

    zend_string *out = zend_string_alloc(end, 0);
    for (size_t i = 0; i < end; i++) {
        char c = src[i];

        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        ZSTR_VAL(out)[i] = c;
    }

    ZSTR_VAL(out)[end] = '\0';
    return out;
}

/* Initialise an empty-but-allocated mime-types HashTable on the config.
 * Set semantics: keys are lowercase mime strings, values are dummy
 * IS_TRUE zvals so zend_hash_str_exists is the only lookup. */
static void http_compression_mime_table_init(HashTable **dst)
{
    ALLOC_HASHTABLE(*dst);
    zend_hash_init(*dst, 16, NULL, ZVAL_PTR_DTOR, 0);
}

/* Populate dst from a NULL-terminated default whitelist. Idempotent:
 * adding a key already present is a no-op for set semantics. */
static void http_compression_mime_table_load_defaults(HashTable *dst)
{
    for (const char **p = http_compression_default_mime_types; *p != NULL; p++) {
        zval one;
        ZVAL_TRUE(&one);
        zend_hash_str_update(dst, *p, strlen(*p), &one);
    }
}
#endif /* HAVE_HTTP_COMPRESSION */

/* Class entry */
zend_class_entry *http_server_config_ce;
static zend_object_handlers http_server_config_handlers;

/* http_server_config_from_obj is defined inline in php_http_server.h. */
#define Z_HTTP_SERVER_CONFIG_P(zv) http_server_config_from_obj(Z_OBJ_P(zv))

/* Helper: Check if config is locked */
static inline bool config_check_locked(const http_server_config_t *config)
{
    if (config->is_locked) {
        zend_throw_exception(http_server_runtime_exception_ce,
            "Cannot modify HttpServerConfig after server has started", 0);
        return true;
    }

    return false;
}

/* Helper: Validate that a user-supplied zend_long timeout in seconds fits
 * into uint32_t. Allows 0 for callers where that has a meaningful
 * semantic (shutdown = no grace period). Returns false and throws on
 * invalid input. */
static inline bool config_validate_timeout(zend_long timeout)
{
    if (timeout < 0) {
        zend_throw_exception(http_server_invalid_argument_exception_ce,
            "Timeout cannot be negative", 0);
        return false;
    }

    if ((zend_ulong)timeout > UINT32_MAX) {
        zend_throw_exception(http_server_invalid_argument_exception_ce,
            "Timeout is too large (max ~136 years in seconds)", 0);
        return false;
    }

    return true;
}

/* Variant that rejects zero. Used for network I/O timeouts where a
 * zero value would mean "immediate timeout" rather than "disabled" —
 * an operator who wants effectively-no timeout must ask for a large
 * finite value explicitly. "No timeout" on a production HTTP server
 * is almost always an accident (slow-loris attacks, zombie sockets),
 * so we force the choice to be deliberate. */
static inline bool config_validate_timeout_positive(zend_long timeout)
{
    if (timeout <= 0) {
        zend_throw_exception(http_server_invalid_argument_exception_ce,
            "Timeout must be >= 1 second. Pass a large finite value "
            "(e.g. 86400) if you want an effectively-unbounded timeout; "
            "zero is not accepted because it means 'immediate timeout', "
            "not 'disabled'.", 0);
        return false;
    }

    if ((zend_ulong)timeout > UINT32_MAX) {
        zend_throw_exception(http_server_invalid_argument_exception_ce,
            "Timeout is too large (max ~136 years in seconds)", 0);
        return false;
    }

    return true;
}

/* Helper: Validate that a path points to a readable regular file.
 * Used at setter time for TLS cert / key so typos and permission
 * mistakes surface immediately instead of during start(), where they
 * produce an opaque OpenSSL error far removed from the misconfigured
 * line. Cross-platform via VCWD_*; no TOCTOU concern — OpenSSL re-opens
 * the file in start() and would fail there too. The purpose here is
 * better diagnostics, not security enforcement. */
static inline bool config_validate_readable_file(const zend_string *path,
                                                 const char *label)
{
    zend_stat_t sb;

    if (VCWD_STAT(ZSTR_VAL(path), &sb) != 0) {
        zend_throw_exception_ex(http_server_invalid_argument_exception_ce, 0,
            "%s not found or not accessible: %s", label, ZSTR_VAL(path));
        return false;
    }

    if (!S_ISREG(sb.st_mode)) {
        zend_throw_exception_ex(http_server_invalid_argument_exception_ce, 0,
            "%s must be a regular file: %s", label, ZSTR_VAL(path));
        return false;
    }

    if (VCWD_ACCESS(ZSTR_VAL(path), R_OK) != 0) {
        zend_throw_exception_ex(http_server_invalid_argument_exception_ce, 0,
            "%s is not readable by the current process: %s",
            label, ZSTR_VAL(path));
        return false;
    }

    return true;
}

/* Helper: Add listener */
static void config_add_listener(http_server_config_t *config, http_listener_type_t type,
                                 zend_string *host, int port, bool tls,
                                 uint32_t protocol_mask)
{
    /* Grow array if needed */
    if (config->listener_count >= config->listener_capacity) {
        size_t new_capacity = config->listener_capacity ? config->listener_capacity * 2 : 4;
        config->listeners = erealloc(config->listeners, new_capacity * sizeof(http_listener_config_t));
        config->listener_capacity = new_capacity;
    }

    http_listener_config_t *listener = &config->listeners[config->listener_count++];
    listener->type = type;
    listener->host = host ? zend_string_copy(host) : NULL;
    listener->port = port;
    listener->tls = tls;
    listener->protocol_mask = protocol_mask;
}

/* {{{ proto HttpServerConfig::__construct(?string $host = null, int $port = 8080) */
ZEND_METHOD(TrueAsync_HttpServerConfig, __construct)
{
    (void)return_value;
    zend_string *host = NULL;
    zend_long port = 8080;

    ZEND_PARSE_PARAMETERS_START(0, 2)
        Z_PARAM_OPTIONAL
        Z_PARAM_STR_OR_NULL(host)
        Z_PARAM_LONG(port)
    ZEND_PARSE_PARAMETERS_END();

    http_server_config_t *config = Z_HTTP_SERVER_CONFIG_P(ZEND_THIS);

    /* Set defaults */
    config->backlog = DEFAULT_BACKLOG;
    config->max_connections = DEFAULT_MAX_CONNECTIONS;
    config->workers = 1;
    config->max_inflight_requests = 0;  /* disabled by default — derived at start() */
    config->read_timeout_s = DEFAULT_READ_TIMEOUT;
    config->write_timeout_s = DEFAULT_WRITE_TIMEOUT;
    config->keepalive_timeout_s = DEFAULT_KEEPALIVE_TIMEOUT;
    config->shutdown_timeout_s = DEFAULT_SHUTDOWN_TIMEOUT;
    config->backpressure_target_ms = DEFAULT_BACKPRESSURE_TARGET_MS;
    config->max_connection_age_ms        = DEFAULT_MAX_CONNECTION_AGE_MS;
    config->max_connection_age_grace_ms  = DEFAULT_MAX_CONN_AGE_GRACE_MS;
    config->drain_spread_ms              = DEFAULT_DRAIN_SPREAD_MS;
    config->drain_cooldown_ms            = DEFAULT_DRAIN_COOLDOWN_MS;
    config->stream_write_buffer_bytes    = DEFAULT_STREAM_WRITE_BUFFER_BYTES;
    config->max_body_size                = DEFAULT_MAX_BODY_SIZE;
    config->h2_static_budget_max         = 0;  /* 0 = auto from memory_limit */
    ZVAL_UNDEF(&config->hot_reload_paths);
    ZVAL_UNDEF(&config->hot_reload_extensions);
    config->hot_reload_debounce_ms       = 300;
    config->hot_reload_max_hold_ms       = 2000;
    config->reload_on_sighup             = false;
    config->http3_idle_timeout_ms        = DEFAULT_HTTP3_IDLE_TIMEOUT_MS;
    config->http3_stream_window_bytes    = DEFAULT_HTTP3_STREAM_WINDOW_BYTES;
    config->http3_max_concurrent_streams = DEFAULT_HTTP3_MAX_CONCURRENT_STREAMS;
    config->http3_peer_connection_budget = DEFAULT_HTTP3_PEER_BUDGET;
    config->http3_socket_buffer_bytes = DEFAULT_HTTP3_SOCKET_BUFFER_BYTES;
    config->reactor_mailbox_capacity = 0;   /* 0 = engine default */
    config->tls_buffer_bytes = DEFAULT_TLS_BUFFER_BYTES;
    config->ws_max_message_size  = DEFAULT_WS_MAX_MESSAGE_SIZE;
    config->ws_max_frame_size    = DEFAULT_WS_MAX_FRAME_SIZE;
    config->ws_ping_interval_ms  = DEFAULT_WS_PING_INTERVAL_MS;
    config->ws_pong_timeout_ms   = DEFAULT_WS_PONG_TIMEOUT_MS;
    config->ws_permessage_deflate = false;
    config->http3_alt_svc_enabled = true;  /* RFC 7838 advertise on by default */
    config->http3_pacing = false;          /* QUIC send pacing — opt-in (#59) */
    config->request_scope = true;          /* Per-request child scope on by default */
    config->write_buffer_size = DEFAULT_WRITE_BUFFER_SIZE;
    config->auto_await_body = true;  /* Default: wait for body on non-multipart */
    ZVAL_UNDEF(&config->bootloader);

    /* Add default listener if host provided */
    if (host) {
        if (port < 1 || port > 65535) {
            zend_throw_exception(http_server_invalid_argument_exception_ce,
                "Port must be between 1 and 65535", 0);
            return;
        }

        config_add_listener(config, LISTENER_TYPE_TCP, host, (int)port, false,
                            HTTP_PROTO_MASK_HTTP1 | HTTP_PROTO_MASK_HTTP2);
    }
}
/* }}} */

/* {{{ proto HttpServerConfig::addListener(string $host, int $port, bool $tls = false): static
 *
 * TCP listener accepting both HTTP/1.1 and HTTP/2 (h2c via preface detection
 * on plaintext, h2 via ALPN on TLS). For protocol-restricted ports use
 * addHttp1Listener / addHttp2Listener / addHttp3Listener instead. */
ZEND_METHOD(TrueAsync_HttpServerConfig, addListener)
{
    zend_string *host;
    zend_long port;
    bool tls = false;

    ZEND_PARSE_PARAMETERS_START(2, 3)
        Z_PARAM_STR(host)
        Z_PARAM_LONG(port)
        Z_PARAM_OPTIONAL
        Z_PARAM_BOOL(tls)
    ZEND_PARSE_PARAMETERS_END();

    http_server_config_t *config = Z_HTTP_SERVER_CONFIG_P(ZEND_THIS);

    if (config_check_locked(config)) {
        return;
    }

    if (port < 1 || port > 65535) {
        zend_throw_exception(http_server_invalid_argument_exception_ce,
            "Port must be between 1 and 65535", 0);
        return;
    }

    config_add_listener(config, LISTENER_TYPE_TCP, host, (int)port, tls,
                        HTTP_PROTO_MASK_HTTP1 | HTTP_PROTO_MASK_HTTP2);

    RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
}
/* }}} */

/* {{{ proto HttpServerConfig::addHttp1Listener(string $host, int $port, bool $tls = false): static
 *
 * HTTP/1.1-only TCP listener. A connection that opens with the HTTP/2
 * preface is handed to llhttp, which emits a compliant 400 Bad Request
 * (RFC 9112) and closes. */
ZEND_METHOD(TrueAsync_HttpServerConfig, addHttp1Listener)
{
    zend_string *host;
    zend_long port;
    bool tls = false;

    ZEND_PARSE_PARAMETERS_START(2, 3)
        Z_PARAM_STR(host)
        Z_PARAM_LONG(port)
        Z_PARAM_OPTIONAL
        Z_PARAM_BOOL(tls)
    ZEND_PARSE_PARAMETERS_END();

    http_server_config_t *config = Z_HTTP_SERVER_CONFIG_P(ZEND_THIS);

    if (config_check_locked(config)) {
        return;
    }

    if (port < 1 || port > 65535) {
        zend_throw_exception(http_server_invalid_argument_exception_ce,
            "Port must be between 1 and 65535", 0);
        return;
    }

    config_add_listener(config, LISTENER_TYPE_TCP, host, (int)port, tls,
                        HTTP_PROTO_MASK_HTTP1);

    RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
}
/* }}} */

/* {{{ proto HttpServerConfig::addHttp2Listener(string $host, int $port, bool $tls = false): static
 *
 * HTTP/2-only listener. With tls=false this is h2c (cleartext HTTP/2):
 * the listener requires the RFC 7540 §3.5 preface and routes anything
 * else into nghttp2's BAD_CLIENT_MAGIC path so the client receives a
 * compliant GOAWAY(PROTOCOL_ERROR). With tls=true the server only
 * advertises h2 over ALPN. */
ZEND_METHOD(TrueAsync_HttpServerConfig, addHttp2Listener)
{
    zend_string *host;
    zend_long port;
    bool tls = false;

    ZEND_PARSE_PARAMETERS_START(2, 3)
        Z_PARAM_STR(host)
        Z_PARAM_LONG(port)
        Z_PARAM_OPTIONAL
        Z_PARAM_BOOL(tls)
    ZEND_PARSE_PARAMETERS_END();

    http_server_config_t *config = Z_HTTP_SERVER_CONFIG_P(ZEND_THIS);

    if (config_check_locked(config)) {
        return;
    }

    if (port < 1 || port > 65535) {
        zend_throw_exception(http_server_invalid_argument_exception_ce,
            "Port must be between 1 and 65535", 0);
        return;
    }

    config_add_listener(config, LISTENER_TYPE_TCP, host, (int)port, tls,
                        HTTP_PROTO_MASK_HTTP2);

    RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
}
/* }}} */

/* {{{ proto HttpServerConfig::addHttp3Listener(string $host, int $port): static */
ZEND_METHOD(TrueAsync_HttpServerConfig, addHttp3Listener)
{
    zend_string *host;
    zend_long port;

    ZEND_PARSE_PARAMETERS_START(2, 2)
        Z_PARAM_STR(host)
        Z_PARAM_LONG(port)
    ZEND_PARSE_PARAMETERS_END();

    http_server_config_t *config = Z_HTTP_SERVER_CONFIG_P(ZEND_THIS);

    if (config_check_locked(config)) {
        return;
    }

    if (port < 1 || port > 65535) {
        zend_throw_exception(http_server_invalid_argument_exception_ce,
            "Port must be between 1 and 65535", 0);
        return;
    }

    /* HTTP/3 mandates TLS 1.3. Flag the listener as TLS so start() builds
     * the shared tls_context_t and requires the cert/key config (same as
     * TCP+TLS listeners). The UDP transport reuses that SSL_CTX for
     * ngtcp2_crypto_ossl per-connection handshake. */
    config_add_listener(config, LISTENER_TYPE_UDP_H3, host, (int)port, true,
                        HTTP_PROTO_MASK_HTTP3);

    RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
}
/* }}} */

/* {{{ proto HttpServerConfig::addUnixListener(string $path): static */
ZEND_METHOD(TrueAsync_HttpServerConfig, addUnixListener)
{
    zend_string *path;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_STR(path)
    ZEND_PARSE_PARAMETERS_END();

    http_server_config_t *config = Z_HTTP_SERVER_CONFIG_P(ZEND_THIS);

    if (config_check_locked(config)) {
        return;
    }

    config_add_listener(config, LISTENER_TYPE_UNIX, path, 0, false,
                        HTTP_PROTO_MASK_HTTP1 | HTTP_PROTO_MASK_HTTP2);

    RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
}
/* }}} */

/* {{{ proto HttpServerConfig::getListeners(): array */
ZEND_METHOD(TrueAsync_HttpServerConfig, getListeners)
{
    ZEND_PARSE_PARAMETERS_NONE();

    http_server_config_t *config = Z_HTTP_SERVER_CONFIG_P(ZEND_THIS);

    array_init(return_value);

    for (size_t i = 0; i < config->listener_count; i++) {
        http_listener_config_t *listener = &config->listeners[i];
        zval entry;
        array_init(&entry);

        if (listener->type == LISTENER_TYPE_TCP) {
            add_assoc_string(&entry, "type", "tcp");
            add_assoc_str(&entry, "host", zend_string_copy(listener->host));
            add_assoc_long(&entry, "port", listener->port);
            add_assoc_bool(&entry, "tls", listener->tls);
            add_assoc_long(&entry, "protocol_mask", (zend_long)listener->protocol_mask);
        } else if (listener->type == LISTENER_TYPE_UDP_H3) {
            add_assoc_string(&entry, "type", "udp_h3");
            add_assoc_str(&entry, "host", zend_string_copy(listener->host));
            add_assoc_long(&entry, "port", listener->port);
            add_assoc_bool(&entry, "tls", listener->tls);
            add_assoc_long(&entry, "protocol_mask", (zend_long)listener->protocol_mask);
        } else {
            add_assoc_string(&entry, "type", "unix");
            add_assoc_str(&entry, "path", zend_string_copy(listener->host));
            add_assoc_long(&entry, "protocol_mask", (zend_long)listener->protocol_mask);
        }

        add_next_index_zval(return_value, &entry);
    }
}
/* }}} */

/* {{{ proto HttpServerConfig::setBacklog(int $backlog): static */
ZEND_METHOD(TrueAsync_HttpServerConfig, setBacklog)
{
    zend_long backlog;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_LONG(backlog)
    ZEND_PARSE_PARAMETERS_END();

    http_server_config_t *config = Z_HTTP_SERVER_CONFIG_P(ZEND_THIS);

    if (config_check_locked(config)) {
        return;
    }

    if (backlog < 1) {
        zend_throw_exception(http_server_invalid_argument_exception_ce,
            "Backlog must be at least 1", 0);
        return;
    }

    config->backlog = (int)backlog;

    RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
}
/* }}} */

/* {{{ proto HttpServerConfig::getBacklog(): int */
ZEND_METHOD(TrueAsync_HttpServerConfig, getBacklog)
{
    ZEND_PARSE_PARAMETERS_NONE();
    http_server_config_t *config = Z_HTTP_SERVER_CONFIG_P(ZEND_THIS);
    RETURN_LONG(config->backlog);
}
/* }}} */

/* {{{ proto HttpServerConfig::setWorkers(int $workers): static
 *
 * Built-in worker pool size (issue #11). 1 (default) = single-threaded;
 * start() runs the event loop on the calling thread. > 1 = HttpServer
 * spawns an Async\ThreadPool of this size at start() time, replicates
 * the config + handler set to each worker via transfer_obj, and the
 * parent's start() awaits all workers' completion. */
ZEND_METHOD(TrueAsync_HttpServerConfig, setWorkers)
{
    zend_long workers;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_LONG(workers)
    ZEND_PARSE_PARAMETERS_END();

    http_server_config_t *config = Z_HTTP_SERVER_CONFIG_P(ZEND_THIS);

    if (config_check_locked(config)) {
        return;
    }

    if (workers < 1 || workers > 1024) {
        zend_throw_exception(http_server_invalid_argument_exception_ce,
            "Workers must be between 1 and 1024", 0);
        return;
    }

    config->workers = (int)workers;

    RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
}
/* }}} */

/* {{{ proto HttpServerConfig::getWorkers(): int */
ZEND_METHOD(TrueAsync_HttpServerConfig, getWorkers)
{
    ZEND_PARSE_PARAMETERS_NONE();
    http_server_config_t *config = Z_HTTP_SERVER_CONFIG_P(ZEND_THIS);
    RETURN_LONG(config->workers);
}
/* }}} */

/* {{{ proto HttpServerConfig::enableHotReload(array $watchPaths, array $extensions = ['php'], int $debounceMs = 300, int $maxHoldMs = 2000): static
 *
 * Dev hot-reload trigger (issue #93), pool mode only. The pool parent spawns
 * one recursive Async\FileSystemWatcher per path with the given debounce; a
 * collapsed change event invalidates the watched trees in opcache and calls
 * HttpServer::reload() — replacement workers re-run the bootloader. */
ZEND_METHOD(TrueAsync_HttpServerConfig, enableHotReload)
{
    HashTable *paths;
    HashTable *extensions = NULL;
    zend_long debounce_ms = 300;
    zend_long max_hold_ms = 2000;

    ZEND_PARSE_PARAMETERS_START(1, 4)
        Z_PARAM_ARRAY_HT(paths)
        Z_PARAM_OPTIONAL
        Z_PARAM_ARRAY_HT(extensions)
        Z_PARAM_LONG(debounce_ms)
        Z_PARAM_LONG(max_hold_ms)
    ZEND_PARSE_PARAMETERS_END();

    http_server_config_t *config = Z_HTTP_SERVER_CONFIG_P(ZEND_THIS);

    if (config_check_locked(config)) {
        return;
    }

    if (zend_hash_num_elements(paths) == 0) {
        zend_throw_exception(http_server_invalid_argument_exception_ce,
            "enableHotReload() requires at least one watch path", 0);
        return;
    }

    const zval *entry;
    ZEND_HASH_FOREACH_VAL(paths, entry)
    {
        if (Z_TYPE_P(entry) != IS_STRING || Z_STRLEN_P(entry) == 0) {
            zend_throw_exception(http_server_invalid_argument_exception_ce,
                "enableHotReload() watch paths must be non-empty strings", 0);
            return;
        }
    }
    ZEND_HASH_FOREACH_END();

    if (debounce_ms < 1 || debounce_ms > 600000 || max_hold_ms < 0 || max_hold_ms > 600000) {
        zend_throw_exception(http_server_invalid_argument_exception_ce,
            "enableHotReload() debounceMs must be 1..600000, maxHoldMs 0..600000", 0);
        return;
    }

    if (Z_TYPE(config->hot_reload_paths) != IS_UNDEF) {
        zval_ptr_dtor(&config->hot_reload_paths);
    }

    if (Z_TYPE(config->hot_reload_extensions) != IS_UNDEF) {
        zval_ptr_dtor(&config->hot_reload_extensions);
        ZVAL_UNDEF(&config->hot_reload_extensions);
    }

    ZVAL_ARR(&config->hot_reload_paths, zend_array_dup(paths));

    if (extensions != NULL && zend_hash_num_elements(extensions) > 0) {
        ZVAL_ARR(&config->hot_reload_extensions, zend_array_dup(extensions));
    }

    config->hot_reload_debounce_ms = (uint32_t)debounce_ms;
    config->hot_reload_max_hold_ms = (uint32_t)max_hold_ms;

    RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
}
/* }}} */

/* {{{ proto HttpServerConfig::enableReloadOnSignal(bool $enabled = true): static
 *
 * Prod hot-reload trigger (issue #93), pool mode only: the pool parent arms a
 * persistent SIGHUP handler that calls HttpServer::reload(). */
ZEND_METHOD(TrueAsync_HttpServerConfig, enableReloadOnSignal)
{
    bool enabled = true;

    ZEND_PARSE_PARAMETERS_START(0, 1)
        Z_PARAM_OPTIONAL
        Z_PARAM_BOOL(enabled)
    ZEND_PARSE_PARAMETERS_END();

    http_server_config_t *config = Z_HTTP_SERVER_CONFIG_P(ZEND_THIS);

    if (config_check_locked(config)) {
        return;
    }

#ifdef PHP_WIN32
    if (enabled) {
        zend_throw_exception(http_server_invalid_argument_exception_ce,
            "enableReloadOnSignal() is not supported on Windows (no SIGHUP)", 0);
        return;
    }
#endif

    config->reload_on_sighup = enabled;

    RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
}
/* }}} */

/* {{{ proto HttpServerConfig::setBootloader(?Closure $bootloader): static
 *
 * Optional bootloader Closure handed to the built-in worker pool. The
 * pool deep-copies it once and runs it on every worker before that
 * worker's task loop — the right place for per-worker autoload, DB
 * pool warm-up, opcache primes, or any other one-shot init that would
 * otherwise need to run inside the handler closure on every request.
 *
 * Only consulted when setWorkers(N) > 1. Pass NULL to clear. */
ZEND_METHOD(TrueAsync_HttpServerConfig, setBootloader)
{
    zval *bootloader_zv = NULL;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_OBJECT_OF_CLASS_OR_NULL(bootloader_zv, zend_ce_closure)
    ZEND_PARSE_PARAMETERS_END();

    http_server_config_t *config = Z_HTTP_SERVER_CONFIG_P(ZEND_THIS);

    if (config_check_locked(config)) {
        return;
    }

    if (Z_TYPE(config->bootloader) != IS_UNDEF) {
        zval_ptr_dtor(&config->bootloader);
        ZVAL_UNDEF(&config->bootloader);
    }

    if (bootloader_zv != NULL) {
        ZVAL_COPY(&config->bootloader, bootloader_zv);
    }

    RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
}
/* }}} */

/* {{{ proto HttpServerConfig::getBootloader(): ?Closure */
ZEND_METHOD(TrueAsync_HttpServerConfig, getBootloader)
{
    ZEND_PARSE_PARAMETERS_NONE();
    http_server_config_t *config = Z_HTTP_SERVER_CONFIG_P(ZEND_THIS);

    if (Z_TYPE(config->bootloader) == IS_UNDEF) {
        RETURN_NULL();
    }

    ZVAL_COPY(return_value, &config->bootloader);
}
/* }}} */

/* {{{ proto HttpServerConfig::setMaxConnections(int $maxConnections): static */
ZEND_METHOD(TrueAsync_HttpServerConfig, setMaxConnections)
{
    zend_long max_connections;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_LONG(max_connections)
    ZEND_PARSE_PARAMETERS_END();

    http_server_config_t *config = Z_HTTP_SERVER_CONFIG_P(ZEND_THIS);

    if (config_check_locked(config)) {
        return;
    }

    if (max_connections < 0) {
        zend_throw_exception(http_server_invalid_argument_exception_ce,
            "Max connections cannot be negative", 0);
        return;
    }

    config->max_connections = (int)max_connections;

    RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
}
/* }}} */

/* {{{ proto HttpServerConfig::getMaxConnections(): int */
ZEND_METHOD(TrueAsync_HttpServerConfig, getMaxConnections)
{
    ZEND_PARSE_PARAMETERS_NONE();
    http_server_config_t *config = Z_HTTP_SERVER_CONFIG_P(ZEND_THIS);
    RETURN_LONG(config->max_connections);
}
/* }}} */

/* {{{ proto HttpServerConfig::setMaxInflightRequests(int $n): static
 *
 * Overload-shedding cap. Once
 * active handler coroutines reach this number, new requests get a
 * fast reject (H1 → 503 + Retry-After, H2 → RST_STREAM REFUSED_STREAM)
 * before any allocations happen. 0 keeps admission disabled; leaving
 * it 0 on start() derives the cap from max_connections * 10.
 */
ZEND_METHOD(TrueAsync_HttpServerConfig, setMaxInflightRequests)
{
    zend_long n;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_LONG(n)
    ZEND_PARSE_PARAMETERS_END();

    http_server_config_t *config = Z_HTTP_SERVER_CONFIG_P(ZEND_THIS);

    if (config_check_locked(config)) {
        return;
    }

    if (n < 0) {
        zend_throw_exception(http_server_invalid_argument_exception_ce,
            "Max in-flight requests cannot be negative", 0);
        return;
    }

    config->max_inflight_requests = (size_t)n;
    RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
}
/* }}} */

/* {{{ proto HttpServerConfig::getMaxInflightRequests(): int */
ZEND_METHOD(TrueAsync_HttpServerConfig, getMaxInflightRequests)
{
    ZEND_PARSE_PARAMETERS_NONE();
    http_server_config_t *config = Z_HTTP_SERVER_CONFIG_P(ZEND_THIS);
    RETURN_LONG((zend_long)config->max_inflight_requests);
}
/* }}} */

/* {{{ proto HttpServerConfig::setReadTimeout(int $timeout): static */
ZEND_METHOD(TrueAsync_HttpServerConfig, setReadTimeout)
{
    zend_long timeout;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_LONG(timeout)
    ZEND_PARSE_PARAMETERS_END();

    http_server_config_t *config = Z_HTTP_SERVER_CONFIG_P(ZEND_THIS);

    if (config_check_locked(config)) {
        return;
    }

    if (!config_validate_timeout_positive(timeout)) {
        return;
    }

    config->read_timeout_s = (uint32_t)timeout;

    RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
}
/* }}} */

/* {{{ proto HttpServerConfig::getReadTimeout(): int */
ZEND_METHOD(TrueAsync_HttpServerConfig, getReadTimeout)
{
    ZEND_PARSE_PARAMETERS_NONE();
    http_server_config_t *config = Z_HTTP_SERVER_CONFIG_P(ZEND_THIS);
    RETURN_LONG(config->read_timeout_s);
}
/* }}} */

/* {{{ proto HttpServerConfig::setWriteTimeout(int $timeout): static */
ZEND_METHOD(TrueAsync_HttpServerConfig, setWriteTimeout)
{
    zend_long timeout;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_LONG(timeout)
    ZEND_PARSE_PARAMETERS_END();

    http_server_config_t *config = Z_HTTP_SERVER_CONFIG_P(ZEND_THIS);

    if (config_check_locked(config)) {
        return;
    }

    /* Write timeout: 0 disables the per-conn write deadline timer.
     * Unlike read (where 0 means "fire immediately" and is dangerous),
     * an unbounded write is bounded by libuv's RST/EOF/EPIPE
     * propagation and the OS TCP retransmit timeout, so disabling is
     * a viable perf knob in trusted-client deployments. */
    if (!config_validate_timeout(timeout)) {
        return;
    }

    config->write_timeout_s = (uint32_t)timeout;

    RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
}
/* }}} */

/* {{{ proto HttpServerConfig::getWriteTimeout(): int */
ZEND_METHOD(TrueAsync_HttpServerConfig, getWriteTimeout)
{
    ZEND_PARSE_PARAMETERS_NONE();
    http_server_config_t *config = Z_HTTP_SERVER_CONFIG_P(ZEND_THIS);
    RETURN_LONG(config->write_timeout_s);
}
/* }}} */

/* {{{ proto HttpServerConfig::setKeepAliveTimeout(int $timeout): static */
ZEND_METHOD(TrueAsync_HttpServerConfig, setKeepAliveTimeout)
{
    zend_long timeout;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_LONG(timeout)
    ZEND_PARSE_PARAMETERS_END();

    http_server_config_t *config = Z_HTTP_SERVER_CONFIG_P(ZEND_THIS);

    if (config_check_locked(config)) {
        return;
    }

    if (!config_validate_timeout_positive(timeout)) {
        return;
    }

    config->keepalive_timeout_s = (uint32_t)timeout;

    RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
}
/* }}} */

/* {{{ proto HttpServerConfig::getKeepAliveTimeout(): int */
ZEND_METHOD(TrueAsync_HttpServerConfig, getKeepAliveTimeout)
{
    ZEND_PARSE_PARAMETERS_NONE();
    http_server_config_t *config = Z_HTTP_SERVER_CONFIG_P(ZEND_THIS);
    RETURN_LONG(config->keepalive_timeout_s);
}
/* }}} */

/* {{{ proto HttpServerConfig::setShutdownTimeout(int $timeout): static */
ZEND_METHOD(TrueAsync_HttpServerConfig, setShutdownTimeout)
{
    zend_long timeout;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_LONG(timeout)
    ZEND_PARSE_PARAMETERS_END();

    http_server_config_t *config = Z_HTTP_SERVER_CONFIG_P(ZEND_THIS);

    if (config_check_locked(config)) {
        return;
    }

    if (!config_validate_timeout(timeout)) {
        return;
    }

    config->shutdown_timeout_s = (uint32_t)timeout;

    RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
}
/* }}} */

/* {{{ proto HttpServerConfig::getShutdownTimeout(): int */
ZEND_METHOD(TrueAsync_HttpServerConfig, getShutdownTimeout)
{
    ZEND_PARSE_PARAMETERS_NONE();
    http_server_config_t *config = Z_HTTP_SERVER_CONFIG_P(ZEND_THIS);
    RETURN_LONG(config->shutdown_timeout_s);
}
/* }}} */

/* {{{ proto HttpServerConfig::setBackpressureTargetMs(int $ms): static
 *
 * Sets the CoDel target sojourn in milliseconds. When request queue-wait
 * (sojourn) stays above this threshold for 100 ms, the listener is paused
 * until existing work drains. 0 disables CoDel (hard cap via
 * max_connections still works). Default: 0 (off) — sojourn-based AQM
 * misfires on HTTP/2 mux where sustained pipelines look like persistent
 * queue. Set to 5 (RFC 8289) to opt in for HTTP/1-dominated workloads.
 *
 * See docs/BACKPRESSURE.md for full mechanism + tuning guidance. */
ZEND_METHOD(TrueAsync_HttpServerConfig, setBackpressureTargetMs)
{
    zend_long ms;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_LONG(ms)
    ZEND_PARSE_PARAMETERS_END();

    http_server_config_t *config = Z_HTTP_SERVER_CONFIG_P(ZEND_THIS);

    if (config_check_locked(config)) {
        return;
    }

    /* Upper bound is generous — operators with very slow handlers
     * (seconds-long background work served via async I/O) may want
     * large values. Anything beyond 10 s is almost certainly a bug. */
    if (ms < 0 || ms > 10000) {
        zend_throw_exception(http_server_invalid_argument_exception_ce,
            "Backpressure target must be between 0 and 10000 ms", 0);
        return;
    }

    config->backpressure_target_ms = (uint32_t)ms;

    RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
}
/* }}} */

/* {{{ proto HttpServerConfig::getBackpressureTargetMs(): int */
ZEND_METHOD(TrueAsync_HttpServerConfig, getBackpressureTargetMs)
{
    ZEND_PARSE_PARAMETERS_NONE();
    http_server_config_t *config = Z_HTTP_SERVER_CONFIG_P(ZEND_THIS);
    RETURN_LONG(config->backpressure_target_ms);
}
/* }}} */

/* {{{ Connection-drain knobs.
 * All four take milliseconds; zero has special meaning per knob (see
 * individual docstrings). Validation rejects sub-second values for the
 * "long-horizon" knobs (age / grace / cooldown) where tiny values are
 * almost always misconfig; spread permits down to 100 ms because very-
 * high-throughput deployments may legitimately want a tighter window.
 */

/* proto HttpServerConfig::setMaxConnectionAgeMs(int $ms): static
 *
 * Proactive drain: after N ms of connection lifetime (± 10% jitter),
 * the connection is marked for graceful close. Next response carries
 * Connection: close (HTTP/1) or the session sends GOAWAY (HTTP/2).
 * Matches gRPC MAX_CONNECTION_AGE semantics.
 *
 * 0 disables the feature. Operator-facing — recommended values start
 * at 600000 (10 min) for services behind L4 load balancers. */
ZEND_METHOD(TrueAsync_HttpServerConfig, setMaxConnectionAgeMs)
{
    zend_long ms;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_LONG(ms)
    ZEND_PARSE_PARAMETERS_END();

    http_server_config_t *config = Z_HTTP_SERVER_CONFIG_P(ZEND_THIS);

    if (config_check_locked(config)) { return; }

    if (ms < 0 || (ms > 0 && ms < 1000)) {
        zend_throw_exception(http_server_invalid_argument_exception_ce,
            "MaxConnectionAge must be 0 (disabled) or >= 1000 ms", 0);
        return;
    }

    config->max_connection_age_ms = (uint32_t)ms;
    RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
}

/* proto HttpServerConfig::getMaxConnectionAgeMs(): int */
ZEND_METHOD(TrueAsync_HttpServerConfig, getMaxConnectionAgeMs)
{
    ZEND_PARSE_PARAMETERS_NONE();
    http_server_config_t *config = Z_HTTP_SERVER_CONFIG_P(ZEND_THIS);
    RETURN_LONG(config->max_connection_age_ms);
}

/* proto HttpServerConfig::setMaxConnectionAgeGraceMs(int $ms): static
 *
 * Hard-close grace after drain is signalled: if the peer hasn't closed
 * the TCP connection within this window, we force-close. 0 = infinite
 * (no force-close timer is armed; we rely on keepalive_timeout_s /
 * read_timeout_s to eventually clean up). Non-zero values must be
 * >= 1000 ms — sub-second timers are a misconfig magnet. */
ZEND_METHOD(TrueAsync_HttpServerConfig, setMaxConnectionAgeGraceMs)
{
    zend_long ms;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_LONG(ms)
    ZEND_PARSE_PARAMETERS_END();

    http_server_config_t *config = Z_HTTP_SERVER_CONFIG_P(ZEND_THIS);

    if (config_check_locked(config)) { return; }

    if (ms < 0 || (ms > 0 && ms < 1000)) {
        zend_throw_exception(http_server_invalid_argument_exception_ce,
            "MaxConnectionAgeGrace must be 0 (infinite) or >= 1000 ms", 0);
        return;
    }

    config->max_connection_age_grace_ms = (uint32_t)ms;
    RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
}

/* proto HttpServerConfig::getMaxConnectionAgeGraceMs(): int */
ZEND_METHOD(TrueAsync_HttpServerConfig, getMaxConnectionAgeGraceMs)
{
    ZEND_PARSE_PARAMETERS_NONE();
    http_server_config_t *config = Z_HTTP_SERVER_CONFIG_P(ZEND_THIS);
    RETURN_LONG(config->max_connection_age_grace_ms);
}

/* proto HttpServerConfig::setDrainSpreadMs(int $ms): static
 *
 * Reactive-drain spread window (HAProxy close-spread-time style). When
 * a drain event fires (CoDel trip or hard-cap transition), per-
 * connection drain effect time is uniformly distributed over [0, ms]
 * to avoid a thundering-herd reconnect burst. Must be >= 100 ms. */
ZEND_METHOD(TrueAsync_HttpServerConfig, setDrainSpreadMs)
{
    zend_long ms;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_LONG(ms)
    ZEND_PARSE_PARAMETERS_END();

    http_server_config_t *config = Z_HTTP_SERVER_CONFIG_P(ZEND_THIS);

    if (config_check_locked(config)) { return; }

    if (ms < 100) {
        zend_throw_exception(http_server_invalid_argument_exception_ce,
            "DrainSpread must be >= 100 ms", 0);
        return;
    }

    config->drain_spread_ms = (uint32_t)ms;
    RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
}

/* proto HttpServerConfig::getDrainSpreadMs(): int */
ZEND_METHOD(TrueAsync_HttpServerConfig, getDrainSpreadMs)
{
    ZEND_PARSE_PARAMETERS_NONE();
    http_server_config_t *config = Z_HTTP_SERVER_CONFIG_P(ZEND_THIS);
    RETURN_LONG(config->drain_spread_ms);
}

/* proto HttpServerConfig::setDrainCooldownMs(int $ms): static
 *
 * Minimum time between two consecutive reactive-drain triggers.
 * Prevents drain oscillation when CoDel flips paused on-off rapidly.
 * Must be >= 1000 ms. */
ZEND_METHOD(TrueAsync_HttpServerConfig, setDrainCooldownMs)
{
    zend_long ms;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_LONG(ms)
    ZEND_PARSE_PARAMETERS_END();

    http_server_config_t *config = Z_HTTP_SERVER_CONFIG_P(ZEND_THIS);

    if (config_check_locked(config)) { return; }

    if (ms < 1000) {
        zend_throw_exception(http_server_invalid_argument_exception_ce,
            "DrainCooldown must be >= 1000 ms", 0);
        return;
    }

    config->drain_cooldown_ms = (uint32_t)ms;
    RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
}

/* proto HttpServerConfig::getDrainCooldownMs(): int */
ZEND_METHOD(TrueAsync_HttpServerConfig, getDrainCooldownMs)
{
    ZEND_PARSE_PARAMETERS_NONE();
    http_server_config_t *config = Z_HTTP_SERVER_CONFIG_P(ZEND_THIS);
    RETURN_LONG(config->drain_cooldown_ms);
}
/* }}} */

/* proto HttpServerConfig::setStreamWriteBufferBytes(int $bytes): static
 *
 * Per-stream staging-buffer high-water mark for streaming responses
 * (HTTP/2). HttpResponse::write() accepts chunks into a per-stream ring
 * until the queued bytes reach this mark, then suspends the handler
 * coroutine until the data provider drains room.
 *
 * This is a MEMORY knob, not a throughput knob: delivery speed is
 * bounded by the HTTP/2 flow-control window regardless. Larger = fewer
 * handler suspend/resume cycles, more RAM per concurrent stream;
 * smaller = tighter memory, more suspends.
 *
 * Default: 262144 (256 KiB). Valid range: 4096 .. 4194304 (4 MiB). */
ZEND_METHOD(TrueAsync_HttpServerConfig, setStreamWriteBufferBytes)
{
    zend_long bytes;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_LONG(bytes)
    ZEND_PARSE_PARAMETERS_END();

    http_server_config_t *config = Z_HTTP_SERVER_CONFIG_P(ZEND_THIS);

    if (config_check_locked(config)) { return; }

    if (bytes < STREAM_WRITE_BUFFER_MIN_BYTES
        || bytes > STREAM_WRITE_BUFFER_MAX_BYTES) {
        zend_throw_exception(http_server_invalid_argument_exception_ce,
            "StreamWriteBufferBytes must be between 4096 and 4194304", 0);
        return;
    }

    config->stream_write_buffer_bytes = (uint32_t)bytes;
    RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
}

/* proto HttpServerConfig::getStreamWriteBufferBytes(): int */
ZEND_METHOD(TrueAsync_HttpServerConfig, getStreamWriteBufferBytes)
{
    ZEND_PARSE_PARAMETERS_NONE();
    http_server_config_t *config = Z_HTTP_SERVER_CONFIG_P(ZEND_THIS);
    RETURN_LONG(config->stream_write_buffer_bytes);
}

/* proto HttpServerConfig::setH2StaticBudgetMax(int $bytes): static
 *
 * Per-worker memory cap for HTTP/2 static-file body buffers (read-ahead
 * chunks + ring queues). 0 = auto (memory_limit/8). Explicit value is
 * still clamped at runtime so the static budget never exceeds
 * memory_limit minus a small reserve for the rest of the worker. */
ZEND_METHOD(TrueAsync_HttpServerConfig, setH2StaticBudgetMax)
{
    zend_long bytes;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_LONG(bytes)
    ZEND_PARSE_PARAMETERS_END();

    http_server_config_t *config = Z_HTTP_SERVER_CONFIG_P(ZEND_THIS);

    if (config_check_locked(config)) { return; }

    if (bytes < 0) {
        zend_throw_exception(http_server_invalid_argument_exception_ce,
            "H2StaticBudgetMax must be >= 0 (0 = auto)", 0);
        return;
    }

    config->h2_static_budget_max = (size_t)bytes;
    RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
}

/* proto HttpServerConfig::getH2StaticBudgetMax(): int  0 = auto */
ZEND_METHOD(TrueAsync_HttpServerConfig, getH2StaticBudgetMax)
{
    ZEND_PARSE_PARAMETERS_NONE();
    http_server_config_t *config = Z_HTTP_SERVER_CONFIG_P(ZEND_THIS);
    RETURN_LONG((zend_long)config->h2_static_budget_max);
}

/* proto HttpServerConfig::setMaxBodySize(int $bytes): static
 *
 * Upper bound for a single request body on both H1 parser and H2
 * session paths. H1 enforces at parse time (413 + connection close),
 * H2 enforces at DATA-frame accumulation (stream RST_STREAM). Value
 * is mirrored into the global parser pool at server start.
 *
 * Default: 10 MiB. Valid: 1024 .. 16 GiB. */
ZEND_METHOD(TrueAsync_HttpServerConfig, setMaxBodySize)
{
    zend_long bytes;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_LONG(bytes)
    ZEND_PARSE_PARAMETERS_END();

    http_server_config_t *config = Z_HTTP_SERVER_CONFIG_P(ZEND_THIS);

    if (config_check_locked(config)) { return; }

    if (bytes < (zend_long)MAX_BODY_SIZE_MIN
        || (zend_ulong)bytes > (zend_ulong)MAX_BODY_SIZE_MAX) {
        zend_throw_exception(http_server_invalid_argument_exception_ce,
            "MaxBodySize must be between 1024 and 17179869184 (16 GiB)", 0);
        return;
    }

    config->max_body_size = (size_t)bytes;
    RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
}

/* proto HttpServerConfig::getMaxBodySize(): int */
ZEND_METHOD(TrueAsync_HttpServerConfig, getMaxBodySize)
{
    ZEND_PARSE_PARAMETERS_NONE();
    http_server_config_t *config = Z_HTTP_SERVER_CONFIG_P(ZEND_THIS);
    RETURN_LONG((zend_long)config->max_body_size);
}

/* === WebSocket knobs ============================== */

/* proto HttpServerConfig::setWsMaxMessageSize(int $bytes): static
 * Reassembled-message cap. Oversize messages → RFC 6455 1009 + close. */
ZEND_METHOD(TrueAsync_HttpServerConfig, setWsMaxMessageSize)
{
    zend_long bytes;
    ZEND_PARSE_PARAMETERS_START(1, 1) Z_PARAM_LONG(bytes) ZEND_PARSE_PARAMETERS_END();
    http_server_config_t *config = Z_HTTP_SERVER_CONFIG_P(ZEND_THIS);
    if (config_check_locked(config)) { return; }
    if (bytes < (zend_long)WS_MAX_MESSAGE_SIZE_MIN
        || (zend_ulong)bytes > (zend_ulong)WS_MAX_MESSAGE_SIZE_MAX) {
        zend_throw_exception(http_server_invalid_argument_exception_ce,
            "WsMaxMessageSize must be between 128 and 268435456 (256 MiB)", 0);
        return;
    }
    config->ws_max_message_size = (uint32_t)bytes;
    RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
}

/* proto HttpServerConfig::getWsMaxMessageSize(): int */
ZEND_METHOD(TrueAsync_HttpServerConfig, getWsMaxMessageSize)
{
    ZEND_PARSE_PARAMETERS_NONE();
    RETURN_LONG((zend_long)Z_HTTP_SERVER_CONFIG_P(ZEND_THIS)->ws_max_message_size);
}

/* proto HttpServerConfig::setWsMaxFrameSize(int $bytes): static
 * Per-frame cap (defence against fragment-flood). */
ZEND_METHOD(TrueAsync_HttpServerConfig, setWsMaxFrameSize)
{
    zend_long bytes;
    ZEND_PARSE_PARAMETERS_START(1, 1) Z_PARAM_LONG(bytes) ZEND_PARSE_PARAMETERS_END();
    http_server_config_t *config = Z_HTTP_SERVER_CONFIG_P(ZEND_THIS);
    if (config_check_locked(config)) { return; }
    if (bytes < (zend_long)WS_MAX_MESSAGE_SIZE_MIN
        || (zend_ulong)bytes > (zend_ulong)WS_MAX_MESSAGE_SIZE_MAX) {
        zend_throw_exception(http_server_invalid_argument_exception_ce,
            "WsMaxFrameSize must be between 128 and 268435456 (256 MiB)", 0);
        return;
    }
    config->ws_max_frame_size = (uint32_t)bytes;
    RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
}

/* proto HttpServerConfig::getWsMaxFrameSize(): int */
ZEND_METHOD(TrueAsync_HttpServerConfig, getWsMaxFrameSize)
{
    ZEND_PARSE_PARAMETERS_NONE();
    RETURN_LONG((zend_long)Z_HTTP_SERVER_CONFIG_P(ZEND_THIS)->ws_max_frame_size);
}

/* proto HttpServerConfig::setWsPingIntervalMs(int $ms): static
 * Server-initiated PING cadence. 0 = no automatic ping. */
ZEND_METHOD(TrueAsync_HttpServerConfig, setWsPingIntervalMs)
{
    zend_long ms;
    ZEND_PARSE_PARAMETERS_START(1, 1) Z_PARAM_LONG(ms) ZEND_PARSE_PARAMETERS_END();
    http_server_config_t *config = Z_HTTP_SERVER_CONFIG_P(ZEND_THIS);
    if (config_check_locked(config)) { return; }
    if (ms < 0 || (zend_ulong)ms > (zend_ulong)UINT32_MAX) {
        zend_throw_exception(http_server_invalid_argument_exception_ce,
            "WsPingIntervalMs must be a non-negative uint32", 0);
        return;
    }
    config->ws_ping_interval_ms = (uint32_t)ms;
    RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
}

/* proto HttpServerConfig::getWsPingIntervalMs(): int */
ZEND_METHOD(TrueAsync_HttpServerConfig, getWsPingIntervalMs)
{
    ZEND_PARSE_PARAMETERS_NONE();
    RETURN_LONG((zend_long)Z_HTTP_SERVER_CONFIG_P(ZEND_THIS)->ws_ping_interval_ms);
}

/* proto HttpServerConfig::setWsPongTimeoutMs(int $ms): static
 * Pong deadline. 0 = no timeout. */
ZEND_METHOD(TrueAsync_HttpServerConfig, setWsPongTimeoutMs)
{
    zend_long ms;
    ZEND_PARSE_PARAMETERS_START(1, 1) Z_PARAM_LONG(ms) ZEND_PARSE_PARAMETERS_END();
    http_server_config_t *config = Z_HTTP_SERVER_CONFIG_P(ZEND_THIS);
    if (config_check_locked(config)) { return; }
    if (ms < 0 || (zend_ulong)ms > (zend_ulong)UINT32_MAX) {
        zend_throw_exception(http_server_invalid_argument_exception_ce,
            "WsPongTimeoutMs must be a non-negative uint32", 0);
        return;
    }
    config->ws_pong_timeout_ms = (uint32_t)ms;
    RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
}

/* proto HttpServerConfig::getWsPongTimeoutMs(): int */
ZEND_METHOD(TrueAsync_HttpServerConfig, getWsPongTimeoutMs)
{
    ZEND_PARSE_PARAMETERS_NONE();
    RETURN_LONG((zend_long)Z_HTTP_SERVER_CONFIG_P(ZEND_THIS)->ws_pong_timeout_ms);
}

/* proto HttpServerConfig::setWsPermessageDeflate(bool $enabled): static
 * RFC 7692 permessage-deflate. Default off (opt-in: CPU cost + the
 * decompression-bomb surface). Negotiated only if the client offers it. */
ZEND_METHOD(TrueAsync_HttpServerConfig, setWsPermessageDeflate)
{
    bool enabled;
    ZEND_PARSE_PARAMETERS_START(1, 1) Z_PARAM_BOOL(enabled) ZEND_PARSE_PARAMETERS_END();
    http_server_config_t *config = Z_HTTP_SERVER_CONFIG_P(ZEND_THIS);
    if (config_check_locked(config)) { return; }
    config->ws_permessage_deflate = enabled;
    RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
}

/* proto HttpServerConfig::getWsPermessageDeflate(): bool */
ZEND_METHOD(TrueAsync_HttpServerConfig, getWsPermessageDeflate)
{
    ZEND_PARSE_PARAMETERS_NONE();
    RETURN_BOOL(Z_HTTP_SERVER_CONFIG_P(ZEND_THIS)->ws_permessage_deflate);
}

/* === HTTP/3 production knobs (NEXT_STEPS.md §5) ============================
 *
 * Four setters expose the QUIC transport-param dials that previously lived
 * only as env vars or hardcoded defaults at connection-create time. Pattern
 * mirrors the existing H1/H2 setters: validate on set, lock after start().
 * `initial_max_data` stays derived from window × streams (nginx-style) — no
 * separate setter. Env-var overrides keep working at connection-create
 * time as ops escape hatches.
 * ========================================================================== */

/* proto HttpServerConfig::setHttp3IdleTimeoutMs(int $ms): static
 *
 * QUIC `max_idle_timeout` (RFC 9000 §10.1). Default 30000. RFC has no
 * upper ceiling; we accept anything fitting in uint32_t (~49 days) so
 * long-poll / SSE-over-H3 deployments are not blocked by an artificial
 * cap. 0 keeps the default rather than disabling — RFC 9000 treats 0
 * as "no idle timeout advertised", which on most stacks falls back to
 * implementation default; we surface that by leaving the connection-
 * side ngtcp2 default in place. */
ZEND_METHOD(TrueAsync_HttpServerConfig, setHttp3IdleTimeoutMs)
{
    zend_long ms;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_LONG(ms)
    ZEND_PARSE_PARAMETERS_END();

    http_server_config_t *config = Z_HTTP_SERVER_CONFIG_P(ZEND_THIS);

    if (config_check_locked(config)) { return; }

    if (ms < 0) {
        zend_throw_exception(http_server_invalid_argument_exception_ce,
            "Http3IdleTimeoutMs cannot be negative", 0);
        return;
    }

    if ((zend_ulong)ms > UINT32_MAX) {
        zend_throw_exception(http_server_invalid_argument_exception_ce,
            "Http3IdleTimeoutMs is too large (max UINT32_MAX ms ~49 days)", 0);
        return;
    }

    config->http3_idle_timeout_ms = (uint32_t)ms;
    RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
}

/* proto HttpServerConfig::getHttp3IdleTimeoutMs(): int */
ZEND_METHOD(TrueAsync_HttpServerConfig, getHttp3IdleTimeoutMs)
{
    ZEND_PARSE_PARAMETERS_NONE();
    http_server_config_t *config = Z_HTTP_SERVER_CONFIG_P(ZEND_THIS);
    RETURN_LONG((zend_long)config->http3_idle_timeout_ms);
}

/* proto HttpServerConfig::setHttp3StreamWindowBytes(int $bytes): static
 *
 * Single-knob control over `initial_max_stream_data_bidi_local`,
 * `_bidi_remote`, and `_uni`. h2o `http3-input-window-size` analogue;
 * direction-specific tuning is rarely needed and the asymmetric variant
 * adds API surface for no real benefit. `initial_max_data` is derived
 * as window × streams (nginx pattern) at connection-create time.
 *
 * Default: 262144 (256 KiB). Valid: 1024 .. 1 GiB. */
ZEND_METHOD(TrueAsync_HttpServerConfig, setHttp3StreamWindowBytes)
{
    zend_long bytes;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_LONG(bytes)
    ZEND_PARSE_PARAMETERS_END();

    http_server_config_t *config = Z_HTTP_SERVER_CONFIG_P(ZEND_THIS);

    if (config_check_locked(config)) { return; }

    if (bytes < (zend_long)HTTP3_STREAM_WINDOW_MIN_BYTES
        || (zend_ulong)bytes > HTTP3_STREAM_WINDOW_MAX_BYTES) {
        zend_throw_exception(http_server_invalid_argument_exception_ce,
            "Http3StreamWindowBytes must be between 1024 and 1073741824 (1 GiB)", 0);
        return;
    }

    config->http3_stream_window_bytes = (uint32_t)bytes;
    RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
}

/* proto HttpServerConfig::getHttp3StreamWindowBytes(): int */
ZEND_METHOD(TrueAsync_HttpServerConfig, getHttp3StreamWindowBytes)
{
    ZEND_PARSE_PARAMETERS_NONE();
    http_server_config_t *config = Z_HTTP_SERVER_CONFIG_P(ZEND_THIS);
    RETURN_LONG((zend_long)config->http3_stream_window_bytes);
}

/* proto HttpServerConfig::setHttp3MaxConcurrentStreams(int $n): static
 *
 * QUIC `initial_max_streams_bidi`. Caps the number of concurrent bidi
 * streams the peer can open. Maps to nginx's `http3_max_concurrent_streams`.
 *
 * Default: 100. Valid: 1 .. 1_000_000. The upper bound is a sanity cap,
 * not an RFC ceiling — at six zeros we are firmly in pathological-config
 * territory regardless. */
ZEND_METHOD(TrueAsync_HttpServerConfig, setHttp3MaxConcurrentStreams)
{
    zend_long n;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_LONG(n)
    ZEND_PARSE_PARAMETERS_END();

    http_server_config_t *config = Z_HTTP_SERVER_CONFIG_P(ZEND_THIS);

    if (config_check_locked(config)) { return; }

    if (n < 1 || (zend_ulong)n > HTTP3_MAX_CONCURRENT_STREAMS_MAX) {
        zend_throw_exception(http_server_invalid_argument_exception_ce,
            "Http3MaxConcurrentStreams must be between 1 and 1000000", 0);
        return;
    }

    config->http3_max_concurrent_streams = (uint32_t)n;
    RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
}

/* proto HttpServerConfig::getHttp3MaxConcurrentStreams(): int */
ZEND_METHOD(TrueAsync_HttpServerConfig, getHttp3MaxConcurrentStreams)
{
    ZEND_PARSE_PARAMETERS_NONE();
    http_server_config_t *config = Z_HTTP_SERVER_CONFIG_P(ZEND_THIS);
    RETURN_LONG((zend_long)config->http3_max_concurrent_streams);
}

/* proto HttpServerConfig::setHttp3PeerConnectionBudget(int $n): static
 *
 * Per-source-IP cap on concurrent QUIC connections. Mitigates handshake
 * slow-loris and amplification by limiting how many connections one
 * peer can pin server-side. Neither h2o nor nginx exposes this — it's
 * our differentiator. Field already lives at http3_listener.c:974;
 * setter just makes it operator-tunable.
 *
 * Default: 16. Valid: 1 .. 4096. */
ZEND_METHOD(TrueAsync_HttpServerConfig, setHttp3PeerConnectionBudget)
{
    zend_long n;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_LONG(n)
    ZEND_PARSE_PARAMETERS_END();

    http_server_config_t *config = Z_HTTP_SERVER_CONFIG_P(ZEND_THIS);

    if (config_check_locked(config)) { return; }

    if (n < 1 || (zend_ulong)n > HTTP3_PEER_BUDGET_MAX) {
        zend_throw_exception(http_server_invalid_argument_exception_ce,
            "Http3PeerConnectionBudget must be between 1 and 4096", 0);
        return;
    }

    config->http3_peer_connection_budget = (uint32_t)n;
    RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
}

/* proto HttpServerConfig::getHttp3PeerConnectionBudget(): int */
ZEND_METHOD(TrueAsync_HttpServerConfig, getHttp3PeerConnectionBudget)
{
    ZEND_PARSE_PARAMETERS_NONE();
    http_server_config_t *config = Z_HTTP_SERVER_CONFIG_P(ZEND_THIS);
    RETURN_LONG((zend_long)config->http3_peer_connection_budget);
}

/* proto HttpServerConfig::setReactorMailboxCapacity(int $slots): static
 *
 * Per-reactor inbound command-mailbox depth (reactor pool / HTTP/3). 0 = engine
 * default. Valid: 0, or 64 .. 1048576. */
ZEND_METHOD(TrueAsync_HttpServerConfig, setReactorMailboxCapacity)
{
    zend_long n;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_LONG(n)
    ZEND_PARSE_PARAMETERS_END();

    http_server_config_t *config = Z_HTTP_SERVER_CONFIG_P(ZEND_THIS);

    if (config_check_locked(config)) { return; }

    if (n != 0 && (n < REACTOR_MAILBOX_CAP_MIN || (zend_ulong)n > REACTOR_MAILBOX_CAP_MAX)) {
        zend_throw_exception(http_server_invalid_argument_exception_ce,
            "ReactorMailboxCapacity must be 0 (default) or between 64 and 1048576", 0);
        return;
    }

    config->reactor_mailbox_capacity = (uint32_t)n;
    RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
}

/* proto HttpServerConfig::getReactorMailboxCapacity(): int */
ZEND_METHOD(TrueAsync_HttpServerConfig, getReactorMailboxCapacity)
{
    ZEND_PARSE_PARAMETERS_NONE();
    http_server_config_t *config = Z_HTTP_SERVER_CONFIG_P(ZEND_THIS);
    RETURN_LONG((zend_long)config->reactor_mailbox_capacity);
}

/* proto HttpServerConfig::setHttp3SocketBufferBytes(int $bytes): static
 *
 * UDP socket receive/send buffer for HTTP/3 listeners. Absorbs inbound
 * bursts so they don't overflow into RcvbufErrors (silent loss → peer
 * PTO). Applied via SO_*BUFFORCE (bypasses net.core.{r,w}mem_max under
 * CAP_NET_ADMIN) with a SO_*BUF fallback the kernel clamps to the sysctl
 * max. 0 = leave the OS default untouched.
 *
 * Default: 8 MiB. Valid: 0 .. 256 MiB. */
ZEND_METHOD(TrueAsync_HttpServerConfig, setHttp3SocketBufferBytes)
{
    zend_long n;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_LONG(n)
    ZEND_PARSE_PARAMETERS_END();

    http_server_config_t *config = Z_HTTP_SERVER_CONFIG_P(ZEND_THIS);

    if (config_check_locked(config)) { return; }

    if (n < 0 || (zend_ulong)n > HTTP3_SOCKET_BUFFER_MAX) {
        zend_throw_exception(http_server_invalid_argument_exception_ce,
            "Http3SocketBufferBytes must be between 0 and 268435456", 0);
        return;
    }

    config->http3_socket_buffer_bytes = (uint32_t)n;
    RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
}

/* proto HttpServerConfig::getHttp3SocketBufferBytes(): int */
ZEND_METHOD(TrueAsync_HttpServerConfig, getHttp3SocketBufferBytes)
{
    ZEND_PARSE_PARAMETERS_NONE();
    http_server_config_t *config = Z_HTTP_SERVER_CONFIG_P(ZEND_THIS);
    RETURN_LONG((zend_long)config->http3_socket_buffer_bytes);
}

/* proto HttpServerConfig::setTlsBufferBytes(int $bytes): static
 *
 * CT-out BIO ring — how much ciphertext OpenSSL can stage per SSL_write
 * before the emit path parks the tail (issue #29). Bigger = fewer
 * syscalls / higher throughput on bodies larger than one TLS record, at
 * ~$bytes more memory per TLS connection; smaller saves memory for
 * mostly-small-response deployments at no RPS cost.
 *
 * The value is rounded UP to a whole number of TLS records (~17 KiB
 * each: 16 KiB payload + AEAD/header overhead) so the ring always holds
 * whole records — e.g. 16384 becomes 17408. Floored at one record,
 * capped at 16. 0 resets to the default (64 KiB). getTlsBufferBytes()
 * returns the effective (rounded) size. */
ZEND_METHOD(TrueAsync_HttpServerConfig, setTlsBufferBytes)
{
    zend_long n;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_LONG(n)
    ZEND_PARSE_PARAMETERS_END();

    http_server_config_t *config = Z_HTTP_SERVER_CONFIG_P(ZEND_THIS);

    if (config_check_locked(config)) { return; }

    if (n < 0 || (zend_ulong)n > TLS_BUFFER_MAX) {
        zend_throw_exception(http_server_invalid_argument_exception_ce,
            "TlsBufferBytes must be between 0 and 278528 (16 TLS records)", 0);
        return;
    }

    if (n == 0) {
        config->tls_buffer_bytes = DEFAULT_TLS_BUFFER_BYTES;
    } else {
        uint32_t records = ((uint32_t)n + TLS_BUFFER_RECORD_BYTES - 1u)
                           / TLS_BUFFER_RECORD_BYTES;

        if (records < 1u) { records = 1u; }

        if (records > TLS_BUFFER_MAX_RECORDS) { records = TLS_BUFFER_MAX_RECORDS; }

        config->tls_buffer_bytes = records * TLS_BUFFER_RECORD_BYTES;
    }

    RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
}

/* proto HttpServerConfig::getTlsBufferBytes(): int — effective (rounded) size. */
ZEND_METHOD(TrueAsync_HttpServerConfig, getTlsBufferBytes)
{
    ZEND_PARSE_PARAMETERS_NONE();
    http_server_config_t *config = Z_HTTP_SERVER_CONFIG_P(ZEND_THIS);
    RETURN_LONG((zend_long)config->tls_buffer_bytes);
}

/* {{{ proto HttpServerConfig::setHttp3AltSvcEnabled(bool $enable): static
 *
 * Toggle the RFC 7838 Alt-Svc header advertisement. Default true.
 * Setting false suppresses the `Alt-Svc: h3=":<port>"; ma=86400` line
 * on H1/H2 responses even when an H3 listener is up — useful while
 * rolling out QUIC behind a load balancer. PHP_HTTP3_DISABLE_ALT_SVC
 * env var still wins as a runtime override.
 *
 * Replaces the env-var-only knob from the original Alt-Svc commit;
 * the var was unreliable on Windows where putenv() doesn't reach
 * proc_open children. */
ZEND_METHOD(TrueAsync_HttpServerConfig, setHttp3AltSvcEnabled)
{
    bool enable;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_BOOL(enable)
    ZEND_PARSE_PARAMETERS_END();

    http_server_config_t *config = Z_HTTP_SERVER_CONFIG_P(ZEND_THIS);

    if (config_check_locked(config)) {
        return;
    }

    config->http3_alt_svc_enabled = enable;
    RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
}

ZEND_METHOD(TrueAsync_HttpServerConfig, isHttp3AltSvcEnabled)
{
    ZEND_PARSE_PARAMETERS_NONE();
    http_server_config_t *config = Z_HTTP_SERVER_CONFIG_P(ZEND_THIS);
    RETURN_BOOL(config->http3_alt_svc_enabled);
}

/* {{{ proto HttpServerConfig::setHttp3Pacing(bool $enable): static
 *
 * Opt-in QUIC send pacing (#59 Phase 2). When on, the H3 drain caps each
 * burst at the congestion controller's send_quantum and spaces packets
 * via ngtcp2's pacing timer — this smooths bulk sends on lossy / rate-
 * limited paths (fewer drops, better goodput), matching quicly/h2o.
 *
 * Default OFF: on a lossless path (loopback, LAN) pacing only adds cost
 * with nothing to gain, so it is not enabled unless the operator knows
 * the deployment runs over a constrained path. */
ZEND_METHOD(TrueAsync_HttpServerConfig, setHttp3Pacing)
{
    bool enable;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_BOOL(enable)
    ZEND_PARSE_PARAMETERS_END();

    http_server_config_t *config = Z_HTTP_SERVER_CONFIG_P(ZEND_THIS);

    if (config_check_locked(config)) {
        return;
    }

    config->http3_pacing = enable;
    RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
}

ZEND_METHOD(TrueAsync_HttpServerConfig, isHttp3Pacing)
{
    ZEND_PARSE_PARAMETERS_NONE();
    http_server_config_t *config = Z_HTTP_SERVER_CONFIG_P(ZEND_THIS);
    RETURN_BOOL(config->http3_pacing);
}

/* {{{ proto HttpServerConfig::setRequestScope(bool $enable): static
 *
 * Per-request child scope (on by default). When on, every request (and
 * every multiplexed H2/H3 stream) runs in its own async scope, giving it
 * an isolated request_context() subtree and a clean drain boundary.
 *
 * Turning it off reuses the connection scope directly, saving two
 * allocations per request — but Async\request_context() then returns
 * null (use the ?-> operator), so only disable it for handlers that
 * never rely on per-request context. */
ZEND_METHOD(TrueAsync_HttpServerConfig, setRequestScope)
{
    bool enable;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_BOOL(enable)
    ZEND_PARSE_PARAMETERS_END();

    http_server_config_t *config = Z_HTTP_SERVER_CONFIG_P(ZEND_THIS);

    if (config_check_locked(config)) {
        return;
    }

    config->request_scope = enable;
    RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
}

ZEND_METHOD(TrueAsync_HttpServerConfig, isRequestScope)
{
    ZEND_PARSE_PARAMETERS_NONE();
    http_server_config_t *config = Z_HTTP_SERVER_CONFIG_P(ZEND_THIS);
    RETURN_BOOL(config->request_scope);
}

/* {{{ proto HttpServerConfig::setStatsEnabled(bool $enabled): static
 *
 * Opt into the cross-worker statistics aggregate (issue #5). Off by default:
 * with it off, no stats slab is allocated and HttpServer::getStats() throws.
 * Distinct from telemetry_enabled (W3C trace-context ingestion). Fixed at
 * start() like the other config. */
ZEND_METHOD(TrueAsync_HttpServerConfig, setStatsEnabled)
{
    bool enabled;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_BOOL(enabled)
    ZEND_PARSE_PARAMETERS_END();

    http_server_config_t *config = Z_HTTP_SERVER_CONFIG_P(ZEND_THIS);

    if (config_check_locked(config)) {
        return;
    }

    config->stats_enabled = enabled;
    RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
}

ZEND_METHOD(TrueAsync_HttpServerConfig, isStatsEnabled)
{
    ZEND_PARSE_PARAMETERS_NONE();
    http_server_config_t *config = Z_HTTP_SERVER_CONFIG_P(ZEND_THIS);
    RETURN_BOOL(config->stats_enabled);
}

/* ==========================================================================
 * HTTP body compression knobs (issue #8). Editable until the config is
 * locked — see HttpServer::__construct. The MIME whitelist setter
 * REPLACES the list wholesale (nginx semantics): pass the full set of
 * eligible types, not a delta against the defaults.
 * ==========================================================================
 */

/* {{{ proto HttpServerConfig::setCompressionEnabled(bool $enable): static */
ZEND_METHOD(TrueAsync_HttpServerConfig, setCompressionEnabled)
{
    bool enable;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_BOOL(enable)
    ZEND_PARSE_PARAMETERS_END();

    http_server_config_t *config = Z_HTTP_SERVER_CONFIG_P(ZEND_THIS);

    if (config_check_locked(config)) return;

#ifdef HAVE_HTTP_COMPRESSION
    config->compression_enabled = enable;
#else
    if (enable) {
        zend_throw_exception(http_server_invalid_argument_exception_ce,
            "HTTP body compression is not built into this extension "
            "(rebuild with --enable-http-compression)", 0);
        return;
    }
#endif
    RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
}
/* }}} */

ZEND_METHOD(TrueAsync_HttpServerConfig, isCompressionEnabled)
{
    ZEND_PARSE_PARAMETERS_NONE();
    http_server_config_t *config = Z_HTTP_SERVER_CONFIG_P(ZEND_THIS);
    RETURN_BOOL(config->compression_enabled);
}

/* {{{ proto HttpServerConfig::setCompressionLevel(int $level): static */
ZEND_METHOD(TrueAsync_HttpServerConfig, setCompressionLevel)
{
    zend_long level;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_LONG(level)
    ZEND_PARSE_PARAMETERS_END();

    http_server_config_t *config = Z_HTTP_SERVER_CONFIG_P(ZEND_THIS);

    if (config_check_locked(config)) return;

#ifdef HAVE_HTTP_COMPRESSION
    if (level < HTTP_COMPRESSION_LEVEL_MIN || level > HTTP_COMPRESSION_LEVEL_MAX) {
        zend_throw_exception_ex(http_server_invalid_argument_exception_ce, 0,
            "Compression level must be between %d and %d",
            HTTP_COMPRESSION_LEVEL_MIN, HTTP_COMPRESSION_LEVEL_MAX);
        return;
    }

    config->compression_level = (uint8_t)level;
#else
    (void)level;
#endif
    RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
}
/* }}} */

ZEND_METHOD(TrueAsync_HttpServerConfig, getCompressionLevel)
{
    ZEND_PARSE_PARAMETERS_NONE();
    http_server_config_t *config = Z_HTTP_SERVER_CONFIG_P(ZEND_THIS);
    RETURN_LONG(config->compression_level);
}

/* {{{ proto HttpServerConfig::setBrotliLevel(int $level): static
 *
 * Brotli quality 0..11. setCompressionLevel keeps its gzip-only meaning
 * — codec level scales differ enough (gzip 1..9, brotli 0..11, zstd 1..22)
 * that a unified setter would lose the high end of brotli/zstd. The
 * setter throws on out-of-range and on a locked config; defaults to 4
 * (production-typical; quality 11 is ~50× slower than 4 with marginal
 * extra ratio). When the extension was built without --enable-brotli
 * the setter is still callable but inert (the response pipeline never
 * picks brotli without HAVE_HTTP_BROTLI). */
ZEND_METHOD(TrueAsync_HttpServerConfig, setBrotliLevel)
{
    zend_long level;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_LONG(level)
    ZEND_PARSE_PARAMETERS_END();

    http_server_config_t *config = Z_HTTP_SERVER_CONFIG_P(ZEND_THIS);

    if (config_check_locked(config)) return;

#ifdef HAVE_HTTP_COMPRESSION
    if (level < HTTP_COMPRESSION_BROTLI_LEVEL_MIN ||
        level > HTTP_COMPRESSION_BROTLI_LEVEL_MAX) {
        zend_throw_exception_ex(http_server_invalid_argument_exception_ce, 0,
            "Brotli level must be between %d and %d",
            HTTP_COMPRESSION_BROTLI_LEVEL_MIN, HTTP_COMPRESSION_BROTLI_LEVEL_MAX);
        return;
    }

    config->brotli_level = (uint8_t)level;
#else
    (void)level;
#endif
    RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
}
/* }}} */

ZEND_METHOD(TrueAsync_HttpServerConfig, getBrotliLevel)
{
    ZEND_PARSE_PARAMETERS_NONE();
    http_server_config_t *config = Z_HTTP_SERVER_CONFIG_P(ZEND_THIS);
    RETURN_LONG(config->brotli_level);
}

/* {{{ proto HttpServerConfig::setZstdLevel(int $level): static
 *
 * zstd compression level 1..22. Default 3 (zstd team's own production
 * default — better ratio than gzip-6 at higher throughput). Levels
 * above ~19 enter "ultra" territory where the encoder reserves
 * substantial extra memory; rarely worthwhile for HTTP payloads. */
ZEND_METHOD(TrueAsync_HttpServerConfig, setZstdLevel)
{
    zend_long level;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_LONG(level)
    ZEND_PARSE_PARAMETERS_END();

    http_server_config_t *config = Z_HTTP_SERVER_CONFIG_P(ZEND_THIS);

    if (config_check_locked(config)) return;

#ifdef HAVE_HTTP_COMPRESSION
    if (level < HTTP_COMPRESSION_ZSTD_LEVEL_MIN ||
        level > HTTP_COMPRESSION_ZSTD_LEVEL_MAX) {
        zend_throw_exception_ex(http_server_invalid_argument_exception_ce, 0,
            "zstd level must be between %d and %d",
            HTTP_COMPRESSION_ZSTD_LEVEL_MIN, HTTP_COMPRESSION_ZSTD_LEVEL_MAX);
        return;
    }

    config->zstd_level = (uint8_t)level;
#else
    (void)level;
#endif
    RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
}
/* }}} */

ZEND_METHOD(TrueAsync_HttpServerConfig, getZstdLevel)
{
    ZEND_PARSE_PARAMETERS_NONE();
    http_server_config_t *config = Z_HTTP_SERVER_CONFIG_P(ZEND_THIS);
    RETURN_LONG(config->zstd_level);
}

/* {{{ proto HttpServerConfig::setJsonEncodeFlags(int $flags): static
 *
 * Default JSON_* flags applied by HttpResponse::json() when the per-call
 * $flags argument is 0. JSON_THROW_ON_ERROR is silently stripped at
 * call time — see ::json() for the rationale (encode failure must yield
 * a 500 JSON error body, not propagate an exception). The setter
 * accepts the flag here for forward-compat (and so callers can OR in
 * the constant freely without having to remember the strip rule), but
 * the bit will not affect actual encode behaviour. */
ZEND_METHOD(TrueAsync_HttpServerConfig, setJsonEncodeFlags)
{
    zend_long flags;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_LONG(flags)
    ZEND_PARSE_PARAMETERS_END();

    http_server_config_t *config = Z_HTTP_SERVER_CONFIG_P(ZEND_THIS);

    if (config_check_locked(config)) return;

    if (flags < 0 || flags > UINT32_MAX) {
        zend_throw_exception(http_server_invalid_argument_exception_ce,
            "JSON encode flags must fit in 32 bits", 0);
        return;
    }

    config->json_encode_flags = (uint32_t)flags;
    RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
}
/* }}} */

ZEND_METHOD(TrueAsync_HttpServerConfig, getJsonEncodeFlags)
{
    ZEND_PARSE_PARAMETERS_NONE();
    http_server_config_t *config = Z_HTTP_SERVER_CONFIG_P(ZEND_THIS);
    RETURN_LONG((zend_long)config->json_encode_flags);
}

/* {{{ proto static HttpServerConfig::getSupportedEncodings(): array
 *
 * Reports the codecs compiled into this build, in server preference
 * order (zstd > br > gzip). Always includes "identity". Tests use this
 * to skip cleanly when the relevant library was not present at build
 * time; production code can use it to surface actual capability without
 * trial-and-error. */
ZEND_METHOD(TrueAsync_HttpServerConfig, getSupportedEncodings)
{
    ZEND_PARSE_PARAMETERS_NONE();
    array_init(return_value);
#ifdef HAVE_HTTP_ZSTD
    add_next_index_string(return_value, "zstd");
#endif
#ifdef HAVE_HTTP_BROTLI
    add_next_index_string(return_value, "br");
#endif
#ifdef HAVE_HTTP_COMPRESSION
    add_next_index_string(return_value, "gzip");
#endif
    add_next_index_string(return_value, "identity");
}
/* }}} */

/* {{{ proto HttpServerConfig::setCompressionMinSize(int $bytes): static */
ZEND_METHOD(TrueAsync_HttpServerConfig, setCompressionMinSize)
{
    zend_long bytes;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_LONG(bytes)
    ZEND_PARSE_PARAMETERS_END();

    http_server_config_t *config = Z_HTTP_SERVER_CONFIG_P(ZEND_THIS);

    if (config_check_locked(config)) return;

#ifdef HAVE_HTTP_COMPRESSION
    if (bytes < 0 || (zend_ulong)bytes > HTTP_COMPRESSION_MIN_SIZE_MAX) {
        zend_throw_exception_ex(http_server_invalid_argument_exception_ce, 0,
            "Compression min-size must be in [0, %u]",
            (unsigned)HTTP_COMPRESSION_MIN_SIZE_MAX);
        return;
    }

    config->compression_min_size = (size_t)bytes;
#else
    (void)bytes;
#endif
    RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
}
/* }}} */

ZEND_METHOD(TrueAsync_HttpServerConfig, getCompressionMinSize)
{
    ZEND_PARSE_PARAMETERS_NONE();
    http_server_config_t *config = Z_HTTP_SERVER_CONFIG_P(ZEND_THIS);
    RETURN_LONG((zend_long)config->compression_min_size);
}

/* {{{ proto HttpServerConfig::setCompressionMimeTypes(array $types): static */
ZEND_METHOD(TrueAsync_HttpServerConfig, setCompressionMimeTypes)
{
    HashTable *types;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_ARRAY_HT(types)
    ZEND_PARSE_PARAMETERS_END();

    http_server_config_t *config = Z_HTTP_SERVER_CONFIG_P(ZEND_THIS);

    if (config_check_locked(config)) return;

#ifdef HAVE_HTTP_COMPRESSION
    /* Pre-validate every entry into a staging HashTable so a malformed
     * element doesn't leave us with a half-replaced policy. nginx
     * semantics: this REPLACES the previous list (defaults included). */
    HashTable staged;
    zend_hash_init(&staged, zend_hash_num_elements(types) + 1, NULL, ZVAL_PTR_DTOR, 0);

    zval *entry;
    ZEND_HASH_FOREACH_VAL(types, entry) {
        if (Z_TYPE_P(entry) != IS_STRING) {
            zend_hash_destroy(&staged);
            zend_throw_exception(http_server_invalid_argument_exception_ce,
                "Compression MIME types must be strings", 0);
            return;
        }

        zend_string *norm = http_compression_normalize_mime(
            Z_STRVAL_P(entry), Z_STRLEN_P(entry));

        if (norm == NULL) {
            zend_hash_destroy(&staged);
            zend_throw_exception(http_server_invalid_argument_exception_ce,
                "Compression MIME type is empty after stripping parameters", 0);
            return;
        }

        zval one;
        ZVAL_TRUE(&one);
        zend_hash_update(&staged, norm, &one);
        zend_string_release(norm);
    } ZEND_HASH_FOREACH_END();

    zend_hash_destroy(config->compression_mime_types);
    /* Re-init in place — preserves the existing pointer that snapshots
     * may already reference internally during config-lock. */
    zend_hash_init(config->compression_mime_types, zend_hash_num_elements(&staged) + 1,
                   NULL, ZVAL_PTR_DTOR, 0);
    zend_string *key;
    ZEND_HASH_FOREACH_STR_KEY(&staged, key) {
        if (key) {
            zval one;
            ZVAL_TRUE(&one);
            zend_hash_update(config->compression_mime_types, key, &one);
        }
    } ZEND_HASH_FOREACH_END();
    zend_hash_destroy(&staged);
#else
    (void)types;
#endif
    RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
}
/* }}} */

ZEND_METHOD(TrueAsync_HttpServerConfig, getCompressionMimeTypes)
{
    ZEND_PARSE_PARAMETERS_NONE();
    http_server_config_t *config = Z_HTTP_SERVER_CONFIG_P(ZEND_THIS);

    array_init(return_value);

    if (config->compression_mime_types) {
        zend_string *key;
        ZEND_HASH_FOREACH_STR_KEY(config->compression_mime_types, key) {
            if (key) {
                add_next_index_str(return_value, zend_string_copy(key));
            }
        } ZEND_HASH_FOREACH_END();
    }
}

/* {{{ proto HttpServerConfig::setRequestMaxDecompressedSize(int $bytes): static */
ZEND_METHOD(TrueAsync_HttpServerConfig, setRequestMaxDecompressedSize)
{
    zend_long bytes;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_LONG(bytes)
    ZEND_PARSE_PARAMETERS_END();

    http_server_config_t *config = Z_HTTP_SERVER_CONFIG_P(ZEND_THIS);

    if (config_check_locked(config)) return;

    if (bytes < 0) {
        zend_throw_exception(http_server_invalid_argument_exception_ce,
            "Max decompressed request body size must be >= 0 "
            "(0 = no cap, must be explicit)", 0);
        return;
    }

    config->request_max_decompressed_size = (size_t)bytes;
    RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
}
/* }}} */

ZEND_METHOD(TrueAsync_HttpServerConfig, getRequestMaxDecompressedSize)
{
    ZEND_PARSE_PARAMETERS_NONE();
    http_server_config_t *config = Z_HTTP_SERVER_CONFIG_P(ZEND_THIS);
    RETURN_LONG((zend_long)config->request_max_decompressed_size);
}

/* {{{ proto HttpServerConfig::setWriteBufferSize(int $size): static */
ZEND_METHOD(TrueAsync_HttpServerConfig, setWriteBufferSize)
{
    zend_long size;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_LONG(size)
    ZEND_PARSE_PARAMETERS_END();

    http_server_config_t *config = Z_HTTP_SERVER_CONFIG_P(ZEND_THIS);

    if (config_check_locked(config)) {
        return;
    }

    if (size < 1024) {
        zend_throw_exception(http_server_invalid_argument_exception_ce,
            "Write buffer size must be at least 1024 bytes", 0);
        return;
    }

    config->write_buffer_size = (size_t)size;

    RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
}
/* }}} */

/* {{{ proto HttpServerConfig::getWriteBufferSize(): int */
ZEND_METHOD(TrueAsync_HttpServerConfig, getWriteBufferSize)
{
    ZEND_PARSE_PARAMETERS_NONE();
    http_server_config_t *config = Z_HTTP_SERVER_CONFIG_P(ZEND_THIS);
    RETURN_LONG((zend_long)config->write_buffer_size);
}
/* }}} */

/* {{{ proto HttpServerConfig::enableHttp2(bool $enable): static */
ZEND_METHOD(TrueAsync_HttpServerConfig, enableHttp2)
{
    bool enable;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_BOOL(enable)
    ZEND_PARSE_PARAMETERS_END();

    http_server_config_t *config = Z_HTTP_SERVER_CONFIG_P(ZEND_THIS);

    if (config_check_locked(config)) {
        return;
    }

    /* enableHttp2() is the legacy flag; the working path is
     * addHttp2Listener() / addHttp2Handler(). Reject the toggle so
     * users land on the supported API rather than a silently-ignored
     * flag. */
    if (enable) {
        zend_throw_exception(http_server_runtime_exception_ce,
            "Use addHttp2Listener() / addHttp2Handler() to enable HTTP/2", 0);
        return;
    }

    config->http2_enabled = enable;

    RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
}
/* }}} */

/* {{{ proto HttpServerConfig::isHttp2Enabled(): bool */
ZEND_METHOD(TrueAsync_HttpServerConfig, isHttp2Enabled)
{
    ZEND_PARSE_PARAMETERS_NONE();
    http_server_config_t *config = Z_HTTP_SERVER_CONFIG_P(ZEND_THIS);
    RETURN_BOOL(config->http2_enabled);
}
/* }}} */

/* {{{ proto HttpServerConfig::enableWebSocket(bool $enable): static */
ZEND_METHOD(TrueAsync_HttpServerConfig, enableWebSocket)
{
    bool enable;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_BOOL(enable)
    ZEND_PARSE_PARAMETERS_END();

    http_server_config_t *config = Z_HTTP_SERVER_CONFIG_P(ZEND_THIS);

    if (config_check_locked(config)) {
        return;
    }

    /* TODO: Implement WebSocket support */
    if (enable) {
        zend_throw_exception(http_server_runtime_exception_ce,
            "WebSocket support is not yet implemented", 0);
        return;
    }

    config->websocket_enabled = enable;

    RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
}
/* }}} */

/* {{{ proto HttpServerConfig::isWebSocketEnabled(): bool */
ZEND_METHOD(TrueAsync_HttpServerConfig, isWebSocketEnabled)
{
    ZEND_PARSE_PARAMETERS_NONE();
    http_server_config_t *config = Z_HTTP_SERVER_CONFIG_P(ZEND_THIS);
    RETURN_BOOL(config->websocket_enabled);
}
/* }}} */

/* {{{ proto HttpServerConfig::enableProtocolDetection(bool $enable): static */
ZEND_METHOD(TrueAsync_HttpServerConfig, enableProtocolDetection)
{
    bool enable;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_BOOL(enable)
    ZEND_PARSE_PARAMETERS_END();

    http_server_config_t *config = Z_HTTP_SERVER_CONFIG_P(ZEND_THIS);

    if (config_check_locked(config)) {
        return;
    }

    config->protocol_detection_enabled = enable;

    RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
}
/* }}} */

/* {{{ proto HttpServerConfig::isProtocolDetectionEnabled(): bool */
ZEND_METHOD(TrueAsync_HttpServerConfig, isProtocolDetectionEnabled)
{
    ZEND_PARSE_PARAMETERS_NONE();
    http_server_config_t *config = Z_HTTP_SERVER_CONFIG_P(ZEND_THIS);
    RETURN_BOOL(config->protocol_detection_enabled);
}
/* }}} */

/* {{{ proto HttpServerConfig::enableTls(bool $enable): static */
ZEND_METHOD(TrueAsync_HttpServerConfig, enableTls)
{
    bool enable;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_BOOL(enable)
    ZEND_PARSE_PARAMETERS_END();

    http_server_config_t *config = Z_HTTP_SERVER_CONFIG_P(ZEND_THIS);

    if (config_check_locked(config)) {
        return;
    }

    /* Flag is persisted; SSL_CTX is built at start() from
     * setCertificate()/setPrivateKey(). Failures to load credentials
     * are surfaced there, not here, to keep config pure. */
    config->tls_enabled = enable;

    RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
}
/* }}} */

/* {{{ proto HttpServerConfig::isTlsEnabled(): bool */
ZEND_METHOD(TrueAsync_HttpServerConfig, isTlsEnabled)
{
    ZEND_PARSE_PARAMETERS_NONE();
    http_server_config_t *config = Z_HTTP_SERVER_CONFIG_P(ZEND_THIS);
    RETURN_BOOL(config->tls_enabled);
}
/* }}} */

/* {{{ proto HttpServerConfig::setCertificate(string $path): static */
ZEND_METHOD(TrueAsync_HttpServerConfig, setCertificate)
{
    zend_string *path;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_STR(path)
    ZEND_PARSE_PARAMETERS_END();

    http_server_config_t *config = Z_HTTP_SERVER_CONFIG_P(ZEND_THIS);

    if (config_check_locked(config)) {
        return;
    }

    if (!config_validate_readable_file(path, "TLS certificate")) {
        return;
    }

    if (config->tls_cert_path) {
        zend_string_release(config->tls_cert_path);
    }

    config->tls_cert_path = zend_string_copy(path);

    RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
}
/* }}} */

/* {{{ proto HttpServerConfig::getCertificate(): ?string */
ZEND_METHOD(TrueAsync_HttpServerConfig, getCertificate)
{
    ZEND_PARSE_PARAMETERS_NONE();
    http_server_config_t *config = Z_HTTP_SERVER_CONFIG_P(ZEND_THIS);

    if (config->tls_cert_path) {
        RETURN_STR_COPY(config->tls_cert_path);
    }

    RETURN_NULL();
}
/* }}} */

/* {{{ proto HttpServerConfig::setPrivateKey(string $path): static */
ZEND_METHOD(TrueAsync_HttpServerConfig, setPrivateKey)
{
    zend_string *path;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_STR(path)
    ZEND_PARSE_PARAMETERS_END();

    http_server_config_t *config = Z_HTTP_SERVER_CONFIG_P(ZEND_THIS);

    if (config_check_locked(config)) {
        return;
    }

    if (!config_validate_readable_file(path, "TLS private key")) {
        return;
    }

    if (config->tls_key_path) {
        zend_string_release(config->tls_key_path);
    }

    config->tls_key_path = zend_string_copy(path);

    RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
}
/* }}} */

/* {{{ proto HttpServerConfig::getPrivateKey(): ?string */
ZEND_METHOD(TrueAsync_HttpServerConfig, getPrivateKey)
{
    ZEND_PARSE_PARAMETERS_NONE();
    http_server_config_t *config = Z_HTTP_SERVER_CONFIG_P(ZEND_THIS);

    if (config->tls_key_path) {
        RETURN_STR_COPY(config->tls_key_path);
    }

    RETURN_NULL();
}
/* }}} */

/* {{{ proto HttpServerConfig::setHttp3HqDocroot(string $path): static */
ZEND_METHOD(TrueAsync_HttpServerConfig, setHttp3HqDocroot)
{
    zend_string *path;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_STR(path)
    ZEND_PARSE_PARAMETERS_END();

    http_server_config_t *config = Z_HTTP_SERVER_CONFIG_P(ZEND_THIS);

    if (config_check_locked(config)) {
        return;
    }

    if (config->http3_hq_docroot) {
        zend_string_release(config->http3_hq_docroot);
    }

    config->http3_hq_docroot = zend_string_copy(path);

    RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
}
/* }}} */

/* {{{ proto HttpServerConfig::getHttp3HqDocroot(): ?string */
ZEND_METHOD(TrueAsync_HttpServerConfig, getHttp3HqDocroot)
{
    ZEND_PARSE_PARAMETERS_NONE();
    http_server_config_t *config = Z_HTTP_SERVER_CONFIG_P(ZEND_THIS);

    if (config->http3_hq_docroot) {
        RETURN_STR_COPY(config->http3_hq_docroot);
    }

    RETURN_NULL();
}
/* }}} */

/* {{{ proto HttpServerConfig::setAutoAwaitBody(bool $enable): static */
ZEND_METHOD(TrueAsync_HttpServerConfig, setAutoAwaitBody)
{
    bool enable;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_BOOL(enable)
    ZEND_PARSE_PARAMETERS_END();

    http_server_config_t *config = Z_HTTP_SERVER_CONFIG_P(ZEND_THIS);

    if (config_check_locked(config)) {
        return;
    }

    config->auto_await_body = enable;

    RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
}
/* }}} */

/* {{{ proto HttpServerConfig::isAutoAwaitBodyEnabled(): bool */
ZEND_METHOD(TrueAsync_HttpServerConfig, isAutoAwaitBodyEnabled)
{
    ZEND_PARSE_PARAMETERS_NONE();
    http_server_config_t *config = Z_HTTP_SERVER_CONFIG_P(ZEND_THIS);
    RETURN_BOOL(config->auto_await_body);
}
/* }}} */

/* {{{ proto HttpServerConfig::isLocked(): bool */
ZEND_METHOD(TrueAsync_HttpServerConfig, isLocked)
{
    ZEND_PARSE_PARAMETERS_NONE();
    http_server_config_t *config = Z_HTTP_SERVER_CONFIG_P(ZEND_THIS);
    RETURN_BOOL(config->is_locked);
}
/* }}} */

/* Logging / telemetry — PLAN_LOG.md.
 * The logger is inactive until both setLogSeverity(>OFF) and
 * setLogStream(<resource>) have been set; activation happens at start(). */

/* {{{ proto HttpServerConfig::setLogSeverity(LogSeverity $level): static */
ZEND_METHOD(TrueAsync_HttpServerConfig, setLogSeverity)
{
    zend_object *level_obj;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_OBJ_OF_CLASS(level_obj, http_log_severity_ce)
    ZEND_PARSE_PARAMETERS_END();

    http_server_config_t *config = Z_HTTP_SERVER_CONFIG_P(ZEND_THIS);

    if (config_check_locked(config)) {
        return;
    }

    /* Backed enum guarantees an IS_LONG case value — Z_LVAL is safe. */
    zval *backing = zend_enum_fetch_case_value(level_obj);
    config->log_severity = (int)Z_LVAL_P(backing);

    RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
}
/* }}} */

/* {{{ proto HttpServerConfig::getLogSeverity(): LogSeverity */
ZEND_METHOD(TrueAsync_HttpServerConfig, getLogSeverity)
{
    ZEND_PARSE_PARAMETERS_NONE();
    http_server_config_t *config = Z_HTTP_SERVER_CONFIG_P(ZEND_THIS);

    zend_object *case_obj = NULL;
    zend_result rc = zend_enum_get_case_by_value(&case_obj, http_log_severity_ce,
                                                 (zend_long)config->log_severity,
                                                 NULL, /* try_from */ false);

    if (rc != SUCCESS || case_obj == NULL) {
        /* Unreachable — log_severity is only ever assigned a sanctioned
         * enum value by setLogSeverity, and the default is OFF. */
        zend_throw_exception(http_server_runtime_exception_ce,
            "Internal: log severity value has no matching enum case", 0);
        return;
    }

    GC_ADDREF(case_obj);
    RETURN_OBJ(case_obj);
}
/* }}} */

/* {{{ proto HttpServerConfig::setLogStream(mixed $stream): static */
ZEND_METHOD(TrueAsync_HttpServerConfig, setLogStream)
{
    zval *stream_zv = NULL;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_ZVAL_OR_NULL(stream_zv)
    ZEND_PARSE_PARAMETERS_END();

    http_server_config_t *config = Z_HTTP_SERVER_CONFIG_P(ZEND_THIS);

    if (config_check_locked(config)) {
        return;
    }

    if (Z_TYPE(config->log_stream) != IS_UNDEF) {
        zval_ptr_dtor(&config->log_stream);
        ZVAL_UNDEF(&config->log_stream);
    }

    if (stream_zv == NULL || Z_TYPE_P(stream_zv) == IS_NULL) {
        RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
    }

    if (Z_TYPE_P(stream_zv) != IS_RESOURCE) {
        zend_throw_exception(http_server_invalid_argument_exception_ce,
            "setLogStream() expects a php_stream resource or null", 0);
        return;
    }

    /* Reject non-stream resources (curl handles, sockets, etc.). */
    php_stream *stream = NULL;
    php_stream_from_zval_no_verify(stream, stream_zv);

    if (stream == NULL) {
        zend_throw_exception(http_server_invalid_argument_exception_ce,
            "setLogStream() expects a php_stream resource (got "
            "an unrelated resource)", 0);
        return;
    }

    ZVAL_COPY(&config->log_stream, stream_zv);

    RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
}
/* }}} */

/* {{{ proto HttpServerConfig::getLogStream(): mixed */
ZEND_METHOD(TrueAsync_HttpServerConfig, getLogStream)
{
    ZEND_PARSE_PARAMETERS_NONE();
    http_server_config_t *config = Z_HTTP_SERVER_CONFIG_P(ZEND_THIS);

    if (Z_TYPE(config->log_stream) == IS_UNDEF) {
        RETURN_NULL();
    }

    ZVAL_COPY(return_value, &config->log_stream);
}
/* }}} */

/* Validate one sink spec array; returns false (with an exception thrown) on
 * any violation. 'type' and 'format' resolve through the sink-type / formatter
 * registry (http_log.c) — a type's own validate() checks its extra keys. The
 * common keys checked here: required LogSeverity 'level', optional 'format'.
 * See http_server_start_logging for how specs build sinks. */
static bool log_sink_spec_valid(zval *elem)
{
    if (Z_TYPE_P(elem) != IS_ARRAY) {
        zend_throw_exception(http_server_invalid_argument_exception_ce,
            "setLogSinks(): each sink must be an array", 0);
        return false;
    }

    HashTable *spec = Z_ARRVAL_P(elem);

    zval *ztype = zend_hash_str_find(spec, "type", sizeof("type") - 1);
    if (ztype == NULL || Z_TYPE_P(ztype) != IS_STRING) {
        zend_throw_exception(http_server_invalid_argument_exception_ce,
            "setLogSinks(): each sink needs a string 'type'", 0);
        return false;
    }

    const http_log_sink_type_t *type =
        http_log_sink_type_by_name(Z_STRVAL_P(ztype), Z_STRLEN_P(ztype));

    if (type == NULL) {
        char names[256];
        http_log_sink_type_names(names, sizeof names);
        zend_throw_exception_ex(http_server_invalid_argument_exception_ce, 0,
            "setLogSinks(): 'type' must be one of %s", names);
        return false;
    }

    if (type->validate != NULL && !type->validate(spec)) {
        return false;   /* exception thrown by the type */
    }

    zval *zfmt = zend_hash_str_find(spec, "format", sizeof("format") - 1);
    if (zfmt != NULL) {
        if (Z_TYPE_P(zfmt) != IS_STRING) {
            zend_throw_exception(http_server_invalid_argument_exception_ce,
                "setLogSinks(): 'format' must be a string", 0);
            return false;
        }

        const http_log_formatter_def_t *fdef =
            http_log_formatter_by_name(Z_STRVAL_P(zfmt), Z_STRLEN_P(zfmt));

        if (fdef == NULL) {
            char names[256];
            http_log_formatter_names(names, sizeof names);
            zend_throw_exception_ex(http_server_invalid_argument_exception_ce, 0,
                "setLogSinks(): 'format' must be one of %s", names);
            return false;
        }

        if (fdef->validate != NULL && !fdef->validate(spec)) {
            return false;   /* exception thrown by the formatter */
        }
    }

    zval *zcat = zend_hash_str_find(spec, "category", sizeof("category") - 1);
    if (zcat != NULL
        && (Z_TYPE_P(zcat) != IS_STRING
            || http_log_category_mask(Z_STRVAL_P(zcat), Z_STRLEN_P(zcat)) == 0)) {
        zend_throw_exception(http_server_invalid_argument_exception_ce,
            "setLogSinks(): 'category' must be one of app|access|all", 0);
        return false;
    }

    zval *zlvl = zend_hash_str_find(spec, "level", sizeof("level") - 1);
    if (zlvl == NULL || Z_TYPE_P(zlvl) != IS_OBJECT
        || !instanceof_function(Z_OBJCE_P(zlvl), http_log_severity_ce)) {
        zend_throw_exception(http_server_invalid_argument_exception_ce,
            "setLogSinks(): each sink needs a LogSeverity 'level'", 0);
        return false;
    }

    return true;
}

/* {{{ proto HttpServerConfig::setLogSinks(array $sinks): static */
ZEND_METHOD(TrueAsync_HttpServerConfig, setLogSinks)
{
    zval *sinks_zv;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_ARRAY(sinks_zv)
    ZEND_PARSE_PARAMETERS_END();

    http_server_config_t *config = Z_HTTP_SERVER_CONFIG_P(ZEND_THIS);

    if (config_check_locked(config)) {
        return;
    }

    HashTable      *ht    = Z_ARRVAL_P(sinks_zv);
    const uint32_t  count = zend_hash_num_elements(ht);

    if (count > HTTP_LOG_MAX_SINKS) {
        zend_throw_exception_ex(http_server_invalid_argument_exception_ce, 0,
            "setLogSinks(): at most %d sinks, got %u", HTTP_LOG_MAX_SINKS, count);
        return;
    }

    zval *elem;
    ZEND_HASH_FOREACH_VAL(ht, elem) {
        if (!log_sink_spec_valid(elem)) {
            return;   /* exception already thrown */
        }
    } ZEND_HASH_FOREACH_END();

    if (Z_TYPE(config->log_sinks) != IS_UNDEF) {
        zval_ptr_dtor(&config->log_sinks);
        ZVAL_UNDEF(&config->log_sinks);
    }

    ZVAL_COPY(&config->log_sinks, sinks_zv);

    RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
}
/* }}} */

/* {{{ proto HttpServerConfig::setTelemetryEnabled(bool $enabled): static */
ZEND_METHOD(TrueAsync_HttpServerConfig, setTelemetryEnabled)
{
    bool enabled;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_BOOL(enabled)
    ZEND_PARSE_PARAMETERS_END();

    http_server_config_t *config = Z_HTTP_SERVER_CONFIG_P(ZEND_THIS);

    if (config_check_locked(config)) {
        return;
    }

    config->telemetry_enabled = enabled;

    RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
}
/* }}} */

/* {{{ proto HttpServerConfig::isTelemetryEnabled(): bool */
ZEND_METHOD(TrueAsync_HttpServerConfig, isTelemetryEnabled)
{
    ZEND_PARSE_PARAMETERS_NONE();
    http_server_config_t *config = Z_HTTP_SERVER_CONFIG_P(ZEND_THIS);
    RETURN_BOOL(config->telemetry_enabled);
}
/* }}} */

/* {{{ proto HttpServerConfig::setBodyStreamingEnabled(bool $enabled): static
 *
 * Issue #26 — when enabled, request bodies are NOT accumulated into
 * req->body; instead DATA chunks are pushed into a per-request queue
 * which the handler drains via HttpRequest::readBody(). Saves up to
 * Content-Length bytes of RSS per concurrent request. Handlers that
 * still call getBody() while streaming is enabled get an empty string. */
ZEND_METHOD(TrueAsync_HttpServerConfig, setBodyStreamingEnabled)
{
    bool enabled;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_BOOL(enabled)
    ZEND_PARSE_PARAMETERS_END();

    http_server_config_t *config = Z_HTTP_SERVER_CONFIG_P(ZEND_THIS);

    if (config_check_locked(config)) {
        return;
    }

    config->body_streaming_enabled = enabled;

    RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
}
/* }}} */

/* {{{ proto HttpServerConfig::isBodyStreamingEnabled(): bool */
ZEND_METHOD(TrueAsync_HttpServerConfig, isBodyStreamingEnabled)
{
    ZEND_PARSE_PARAMETERS_NONE();
    const http_server_config_t *config = Z_HTTP_SERVER_CONFIG_P(ZEND_THIS);
    RETURN_BOOL(config->body_streaming_enabled);
}
/* }}} */

/* Object handlers */

static zend_object *http_server_config_create(zend_class_entry *ce)
{
    http_server_config_t *config = zend_object_alloc(sizeof(http_server_config_t), ce);

    /* Initialize to zero/defaults */
    config->listeners = NULL;
    config->listener_count = 0;
    config->listener_capacity = 0;
    config->backlog = 0;
    config->max_connections = 0;
    config->read_timeout_s = 0;
    config->write_timeout_s = 0;
    config->keepalive_timeout_s = 0;
    config->shutdown_timeout_s = 0;
    config->backpressure_target_ms = 0;
    config->max_connection_age_ms       = 0;
    config->max_connection_age_grace_ms = 0;
    config->drain_spread_ms             = 0;
    config->drain_cooldown_ms           = 0;
    config->stream_write_buffer_bytes   = 0;
    config->max_body_size               = 0;
    config->h2_static_budget_max        = 0;
    config->http3_idle_timeout_ms        = 0;
    config->http3_stream_window_bytes    = 0;
    config->http3_max_concurrent_streams = 0;
    config->http3_peer_connection_budget = 0;
    config->http3_socket_buffer_bytes = 0;
    config->reactor_mailbox_capacity = 0;
    config->tls_buffer_bytes = 0;
    config->ws_max_message_size          = 0;
    config->ws_max_frame_size            = 0;
    config->ws_ping_interval_ms          = 0;
    config->ws_pong_timeout_ms           = 0;
    config->ws_permessage_deflate        = false;
    config->http3_alt_svc_enabled = true;
    config->http3_pacing = false;
    config->request_scope = true;
    config->write_buffer_size = 0;
    config->http2_enabled = false;
    config->websocket_enabled = false;
    config->protocol_detection_enabled = false;
    config->tls_enabled = false;
    config->tls_cert_path = NULL;
    config->tls_key_path = NULL;
    config->http3_hq_docroot = NULL;
    config->auto_await_body = false;
    config->is_locked = false;
    config->log_severity = 0;          /* HTTP_LOG_OFF */
    ZVAL_UNDEF(&config->log_stream);
    ZVAL_UNDEF(&config->log_sinks);
    ZVAL_UNDEF(&config->bootloader);
    config->telemetry_enabled = false;
    config->body_streaming_enabled = false;
    config->frozen = NULL;

#ifdef HAVE_HTTP_COMPRESSION
    config->compression_enabled        = true;
    config->compression_level          = HTTP_COMPRESSION_DEFAULT_LEVEL;
    config->brotli_level               = HTTP_COMPRESSION_BROTLI_DEFAULT_LEVEL;
    config->zstd_level                 = HTTP_COMPRESSION_ZSTD_DEFAULT_LEVEL;
    config->compression_min_size       = HTTP_COMPRESSION_DEFAULT_MIN_SIZE;
    config->request_max_decompressed_size = HTTP_COMPRESSION_DEFAULT_REQUEST_MAX_DECOMP;
    http_compression_mime_table_init(&config->compression_mime_types);
    http_compression_mime_table_load_defaults(config->compression_mime_types);
#else
    config->compression_enabled        = false;
    config->compression_level          = 0;
    config->brotli_level               = 0;
    config->zstd_level                 = 0;
    config->compression_min_size       = 0;
    config->request_max_decompressed_size = 0;
    config->compression_mime_types     = NULL;
#endif

    /* JSON encode defaults — UNESCAPED_UNICODE + UNESCAPED_SLASHES. The
     * pair is the right default for ~every JSON API (smaller bytes on
     * the wire, readable URLs in payloads). */
    config->json_encode_flags = PHP_JSON_UNESCAPED_UNICODE | PHP_JSON_UNESCAPED_SLASHES;

    zend_object_std_init(&config->std, ce);
    object_properties_init(&config->std, ce);
    config->std.handlers = &http_server_config_handlers;

    return &config->std;
}

static void http_server_config_free(zend_object *obj)
{
    http_server_config_t *config = http_server_config_from_obj(obj);

    if (config->listeners) {
        for (size_t i = 0; i < config->listener_count; i++) {
            if (config->listeners[i].host) {
                zend_string_release(config->listeners[i].host);
            }
        }

        efree(config->listeners);
    }

    /* Free TLS paths */
    if (config->tls_cert_path) {
        zend_string_release(config->tls_cert_path);
    }

    if (config->tls_key_path) {
        zend_string_release(config->tls_key_path);
    }

    if (config->http3_hq_docroot) {
        zend_string_release(config->http3_hq_docroot);
    }

    if (Z_TYPE(config->log_stream) != IS_UNDEF) {
        zval_ptr_dtor(&config->log_stream);
        ZVAL_UNDEF(&config->log_stream);
    }

    if (Z_TYPE(config->log_sinks) != IS_UNDEF) {
        zval_ptr_dtor(&config->log_sinks);
        ZVAL_UNDEF(&config->log_sinks);
    }

    if (Z_TYPE(config->hot_reload_paths) != IS_UNDEF) {
        zval_ptr_dtor(&config->hot_reload_paths);
        ZVAL_UNDEF(&config->hot_reload_paths);
    }

    if (Z_TYPE(config->hot_reload_extensions) != IS_UNDEF) {
        zval_ptr_dtor(&config->hot_reload_extensions);
        ZVAL_UNDEF(&config->hot_reload_extensions);
    }

    if (Z_TYPE(config->bootloader) != IS_UNDEF) {
        zval_ptr_dtor(&config->bootloader);
        ZVAL_UNDEF(&config->bootloader);
    }

    /* Release our ref on the frozen snapshot (may destroy it if last). */
    if (config->frozen) {
        http_server_shared_config_release(config->frozen);
        config->frozen = NULL;
    }

    if (config->compression_mime_types) {
        zend_hash_destroy(config->compression_mime_types);
        FREE_HASHTABLE(config->compression_mime_types);
        config->compression_mime_types = NULL;
    }

    zend_object_std_dtor(&config->std);
}

/* ==========================================================================
 * Persistent shared-config lifecycle + cross-thread transfer
 *
 * A locked HttpServerConfig is "frozen" into a pemalloc http_server_shared_config_t
 * with its own atomic refcount. Deep-copying strings / listeners into persistent
 * memory up-front lets transfer_obj collapse to a pointer copy + addref — cheap,
 * and avoids walking user data on the hot path. Each PHP thread that LOADs the
 * config takes a ref; the struct is destroyed when the last ref drops.
 * ==========================================================================
 */

/* Persistent copy of a string spec key, or NULL when absent. */
static zend_string *shared_spec_str(HashTable *spec, const char *key, size_t klen)
{
    zval *z = zend_hash_str_find(spec, key, klen);

    if (z == NULL || Z_TYPE_P(z) != IS_STRING) {
        return NULL;
    }

    zend_string *s = zend_string_init(Z_STRVAL_P(z), Z_STRLEN_P(z), 1);
    GC_MAKE_PERSISTENT_LOCAL(s);
    return s;
}

static http_server_shared_config_t *http_server_shared_config_freeze(
    const http_server_config_t *src)
{
    http_server_shared_config_t *shared = pecalloc(1, sizeof(*shared), 1);

    zend_atomic_int_store(&shared->ref_count, 1);

    shared->backlog            = src->backlog;
    shared->max_connections    = src->max_connections;
    shared->workers            = src->workers;
    shared->max_inflight_requests = src->max_inflight_requests;
    shared->read_timeout_s       = src->read_timeout_s;
    shared->write_timeout_s      = src->write_timeout_s;
    shared->keepalive_timeout_s  = src->keepalive_timeout_s;
    shared->shutdown_timeout_s   = src->shutdown_timeout_s;
    shared->backpressure_target_ms      = src->backpressure_target_ms;
    shared->max_connection_age_ms       = src->max_connection_age_ms;
    shared->max_connection_age_grace_ms = src->max_connection_age_grace_ms;
    shared->drain_spread_ms             = src->drain_spread_ms;
    shared->drain_cooldown_ms           = src->drain_cooldown_ms;
    shared->stream_write_buffer_bytes   = src->stream_write_buffer_bytes;
    shared->max_body_size               = src->max_body_size;
    shared->h2_static_budget_max        = src->h2_static_budget_max;
    shared->ws_max_message_size          = src->ws_max_message_size;
    shared->ws_max_frame_size            = src->ws_max_frame_size;
    shared->ws_ping_interval_ms          = src->ws_ping_interval_ms;
    shared->ws_pong_timeout_ms           = src->ws_pong_timeout_ms;
    shared->ws_permessage_deflate        = src->ws_permessage_deflate;
    shared->http3_idle_timeout_ms        = src->http3_idle_timeout_ms;
    shared->http3_stream_window_bytes    = src->http3_stream_window_bytes;
    shared->http3_max_concurrent_streams = src->http3_max_concurrent_streams;
    shared->http3_peer_connection_budget = src->http3_peer_connection_budget;
    shared->http3_socket_buffer_bytes = src->http3_socket_buffer_bytes;
    shared->reactor_mailbox_capacity = src->reactor_mailbox_capacity;
    shared->tls_buffer_bytes = src->tls_buffer_bytes;
    shared->http3_alt_svc_enabled        = src->http3_alt_svc_enabled;
    shared->http3_pacing                 = src->http3_pacing;
    shared->request_scope                = src->request_scope;
    shared->stats_enabled                = src->stats_enabled;
    shared->write_buffer_size  = src->write_buffer_size;

    shared->http2_enabled              = src->http2_enabled;
    shared->websocket_enabled          = src->websocket_enabled;
    shared->protocol_detection_enabled = src->protocol_detection_enabled;
    shared->tls_enabled                = src->tls_enabled;
    shared->auto_await_body            = src->auto_await_body;
    shared->body_streaming_enabled     = src->body_streaming_enabled;

    shared->compression_enabled           = src->compression_enabled;
    shared->compression_level             = src->compression_level;
    shared->brotli_level                  = src->brotli_level;
    shared->zstd_level                    = src->zstd_level;
    shared->compression_min_size          = src->compression_min_size;
    shared->request_max_decompressed_size = src->request_max_decompressed_size;
    shared->json_encode_flags             = src->json_encode_flags;

    if (src->compression_mime_types && zend_hash_num_elements(src->compression_mime_types) > 0) {
        size_t n = zend_hash_num_elements(src->compression_mime_types);
        shared->compression_mime_types = pecalloc(n, sizeof(zend_string *), 1);
        shared->compression_mime_count = n;
        size_t i = 0;
        zend_string *key;
        ZEND_HASH_FOREACH_STR_KEY(src->compression_mime_types, key) {
            if (key) {
                shared->compression_mime_types[i] = zend_string_init(
                    ZSTR_VAL(key), ZSTR_LEN(key), 1);
                GC_MAKE_PERSISTENT_LOCAL(shared->compression_mime_types[i]);
                i++;
            }
        } ZEND_HASH_FOREACH_END();
        shared->compression_mime_count = i;
    }

    if (src->tls_cert_path) {
        shared->tls_cert_path = zend_string_init(
            ZSTR_VAL(src->tls_cert_path), ZSTR_LEN(src->tls_cert_path), 1);
        GC_MAKE_PERSISTENT_LOCAL(shared->tls_cert_path);
    }

    if (src->tls_key_path) {
        shared->tls_key_path = zend_string_init(
            ZSTR_VAL(src->tls_key_path), ZSTR_LEN(src->tls_key_path), 1);
        GC_MAKE_PERSISTENT_LOCAL(shared->tls_key_path);
    }

    if (src->http3_hq_docroot) {
        shared->http3_hq_docroot = zend_string_init(
            ZSTR_VAL(src->http3_hq_docroot), ZSTR_LEN(src->http3_hq_docroot), 1);
        GC_MAKE_PERSISTENT_LOCAL(shared->http3_hq_docroot);
    }

    if (src->listener_count > 0) {
        shared->listeners = pecalloc(src->listener_count, sizeof(http_listener_shared_t), 1);
        shared->listener_count = src->listener_count;
        for (size_t i = 0; i < src->listener_count; i++) {
            shared->listeners[i].type          = src->listeners[i].type;
            shared->listeners[i].port          = src->listeners[i].port;
            shared->listeners[i].tls           = src->listeners[i].tls;
            shared->listeners[i].protocol_mask = src->listeners[i].protocol_mask;

            if (src->listeners[i].host) {
                shared->listeners[i].host = zend_string_init(
                    ZSTR_VAL(src->listeners[i].host),
                    ZSTR_LEN(src->listeners[i].host), 1);
                GC_MAKE_PERSISTENT_LOCAL(shared->listeners[i].host);
            }
        }
    }

    if (Z_TYPE(src->log_sinks) == IS_ARRAY
        && zend_hash_num_elements(Z_ARRVAL(src->log_sinks)) > 0) {
        const uint32_t total = zend_hash_num_elements(Z_ARRVAL(src->log_sinks));

        shared->log_sinks = pecalloc(total, sizeof(*shared->log_sinks), 1);

        zval *elem;
        ZEND_HASH_FOREACH_VAL(Z_ARRVAL(src->log_sinks), elem) {
            HashTable *spec  = Z_ARRVAL_P(elem);
            zval      *ztype = zend_hash_str_find(spec, "type", sizeof("type") - 1);
            const char *t    = Z_STRVAL_P(ztype);
            const size_t tl  = Z_STRLEN_P(ztype);

            /* A parent-opened resource / closure cannot cross into worker
             * threads. Workers just skip these sinks (they still work on
             * the parent); say so once at freeze time — but only when a
             * pool will actually exist. */
            if ((tl == 6 && memcmp(t, "stream", 6) == 0)
                || (tl == 3 && memcmp(t, "php", 3) == 0)) {
                if (src->workers > 1) {
                    fprintf(stderr,
                            "http_server: log sink type '%.*s' is per-thread and "
                            "will be inactive in pool workers (use 'file'/'stdout'/"
                            "'stderr'/'syslog')\n",
                            (int)tl, t);
                }
                continue;
            }

            struct http_shared_log_sink *dst =
                &shared->log_sinks[shared->log_sink_count];

            dst->type     = shared_spec_str(spec, "type", sizeof("type") - 1);
            dst->format   = shared_spec_str(spec, "format", sizeof("format") - 1);
            dst->target   = shared_spec_str(spec, "target", sizeof("target") - 1);
            dst->facility = shared_spec_str(spec, "facility", sizeof("facility") - 1);
            dst->path     = shared_spec_str(spec, "path", sizeof("path") - 1);
            dst->tmpl     = shared_spec_str(spec, "template", sizeof("template") - 1);
            dst->category = shared_spec_str(spec, "category", sizeof("category") - 1);

            zval *zlvl = zend_hash_str_find(spec, "level", sizeof("level") - 1);
            dst->level = (int)Z_LVAL_P(zend_enum_fetch_case_value(Z_OBJ_P(zlvl)));

            shared->log_sink_count++;
        } ZEND_HASH_FOREACH_END();
    }

    return shared;
}

static void http_server_shared_config_addref(http_server_shared_config_t *shared)
{
    int old;
    do {
        old = zend_atomic_int_load(&shared->ref_count);
    } while (!zend_atomic_int_compare_exchange(&shared->ref_count, &old, old + 1));
}

static void http_server_shared_config_release(http_server_shared_config_t *shared)
{
    int old;
    do {
        old = zend_atomic_int_load(&shared->ref_count);
    } while (!zend_atomic_int_compare_exchange(&shared->ref_count, &old, old - 1));

    if (old > 1) {
        return;
    }

    /* Last ref — tear down. Persistent zend_strings: release only if the
     * string isn't shared further; since we zend_string_init'd them with
     * persistent=1 and GC_MAKE_PERSISTENT_LOCAL, zend_string_release_ex(..., 1)
     * is the correct deallocator. */
    for (size_t i = 0; i < shared->listener_count; i++) {
        if (shared->listeners[i].host) {
            zend_string_release_ex(shared->listeners[i].host, 1);
        }
    }

    if (shared->listeners) {
        pefree(shared->listeners, 1);
    }

    if (shared->tls_cert_path) {
        zend_string_release_ex(shared->tls_cert_path, 1);
    }

    if (shared->tls_key_path) {
        zend_string_release_ex(shared->tls_key_path, 1);
    }

    if (shared->http3_hq_docroot) {
        zend_string_release_ex(shared->http3_hq_docroot, 1);
    }

    if (shared->compression_mime_types) {
        for (size_t i = 0; i < shared->compression_mime_count; i++) {
            if (shared->compression_mime_types[i]) {
                zend_string_release_ex(shared->compression_mime_types[i], 1);
            }
        }

        pefree(shared->compression_mime_types, 1);
    }

    if (shared->log_sinks) {
        for (size_t i = 0; i < shared->log_sink_count; i++) {
            struct http_shared_log_sink *s = &shared->log_sinks[i];
            zend_string *fields[] = { s->type, s->format, s->target,
                                      s->facility, s->path, s->tmpl,
                                      s->category };

            for (size_t k = 0; k < sizeof fields / sizeof fields[0]; k++) {
                if (fields[k] != NULL) {
                    zend_string_release_ex(fields[k], 1);
                }
            }
        }

        pefree(shared->log_sinks, 1);
    }

    pefree(shared, 1);
}

/* Fill an emalloc http_server_config_t from a frozen snapshot. Used by
 * transfer_obj LOAD — the new PHP object owns emalloc copies of strings
 * (so getters that call zend_string_copy work as usual) and holds a ref on
 * the shared struct (so future transfers from this thread stay cheap). */
static void http_server_config_populate_from_shared(
    http_server_config_t *dst, const http_server_shared_config_t *src)
{
    dst->backlog            = src->backlog;
    dst->max_connections    = src->max_connections;
    dst->workers            = src->workers;
    dst->max_inflight_requests = src->max_inflight_requests;
    dst->read_timeout_s       = src->read_timeout_s;
    dst->write_timeout_s      = src->write_timeout_s;
    dst->keepalive_timeout_s  = src->keepalive_timeout_s;
    dst->shutdown_timeout_s   = src->shutdown_timeout_s;
    dst->backpressure_target_ms      = src->backpressure_target_ms;
    dst->max_connection_age_ms       = src->max_connection_age_ms;
    dst->max_connection_age_grace_ms = src->max_connection_age_grace_ms;
    dst->drain_spread_ms             = src->drain_spread_ms;
    dst->drain_cooldown_ms           = src->drain_cooldown_ms;
    dst->stream_write_buffer_bytes   = src->stream_write_buffer_bytes;
    dst->max_body_size               = src->max_body_size;
    dst->h2_static_budget_max        = src->h2_static_budget_max;
    dst->ws_max_message_size          = src->ws_max_message_size;
    dst->ws_max_frame_size            = src->ws_max_frame_size;
    dst->ws_ping_interval_ms          = src->ws_ping_interval_ms;
    dst->ws_pong_timeout_ms           = src->ws_pong_timeout_ms;
    dst->ws_permessage_deflate        = src->ws_permessage_deflate;
    dst->http3_idle_timeout_ms        = src->http3_idle_timeout_ms;
    dst->http3_stream_window_bytes    = src->http3_stream_window_bytes;
    dst->http3_max_concurrent_streams = src->http3_max_concurrent_streams;
    dst->http3_peer_connection_budget = src->http3_peer_connection_budget;
    dst->http3_socket_buffer_bytes = src->http3_socket_buffer_bytes;
    dst->reactor_mailbox_capacity = src->reactor_mailbox_capacity;
    dst->tls_buffer_bytes = src->tls_buffer_bytes;
    dst->http3_alt_svc_enabled        = src->http3_alt_svc_enabled;
    dst->http3_pacing                 = src->http3_pacing;
    dst->request_scope                = src->request_scope;
    dst->stats_enabled                = src->stats_enabled;
    dst->write_buffer_size  = src->write_buffer_size;

    dst->http2_enabled              = src->http2_enabled;
    dst->websocket_enabled          = src->websocket_enabled;
    dst->protocol_detection_enabled = src->protocol_detection_enabled;
    dst->tls_enabled                = src->tls_enabled;
    dst->auto_await_body            = src->auto_await_body;
    dst->body_streaming_enabled     = src->body_streaming_enabled;

    dst->compression_enabled           = src->compression_enabled;
    dst->compression_level             = src->compression_level;
    dst->brotli_level                  = src->brotli_level;
    dst->zstd_level                    = src->zstd_level;
    dst->compression_min_size          = src->compression_min_size;
    dst->request_max_decompressed_size = src->request_max_decompressed_size;
    dst->json_encode_flags             = src->json_encode_flags;

    if (dst->compression_mime_types) {
        /* create_object pre-populated this with defaults; replace with the
         * actual locked snapshot. */
        zend_hash_clean(dst->compression_mime_types);
        for (size_t i = 0; i < src->compression_mime_count; i++) {
            zval one;
            ZVAL_TRUE(&one);
            zend_hash_str_update(dst->compression_mime_types,
                ZSTR_VAL(src->compression_mime_types[i]),
                ZSTR_LEN(src->compression_mime_types[i]),
                &one);
        }
    }

    /* Rebuild the log-sink spec array from the flattened snapshot so this
     * worker's start_logging opens its own transports. 'level' is rebuilt
     * as the LogSeverity case matching the frozen backing value. */
    if (src->log_sink_count > 0) {
        array_init(&dst->log_sinks);

        for (size_t i = 0; i < src->log_sink_count; i++) {
            const struct http_shared_log_sink *s = &src->log_sinks[i];
            const struct { const char *key; zend_string *val; } strs[] = {
                { "type", s->type },         { "format", s->format },
                { "target", s->target },     { "facility", s->facility },
                { "path", s->path },         { "template", s->tmpl },
                { "category", s->category },
            };

            zval spec;
            array_init(&spec);

            for (size_t k = 0; k < sizeof strs / sizeof strs[0]; k++) {
                if (strs[k].val != NULL) {
                    add_assoc_stringl(&spec, strs[k].key,
                                      ZSTR_VAL(strs[k].val),
                                      ZSTR_LEN(strs[k].val));
                }
            }

            const char *case_name = s->level == 0  ? "OFF"
                                  : s->level == 5  ? "DEBUG"
                                  : s->level == 13 ? "WARN"
                                  : s->level == 17 ? "ERROR" : "INFO";
            zval lvl;
            ZVAL_OBJ_COPY(&lvl,
                zend_enum_get_case_cstr(http_log_severity_ce, case_name));
            add_assoc_zval(&spec, "level", &lvl);

            add_next_index_zval(&dst->log_sinks, &spec);
        }
    }

    if (src->tls_cert_path) {
        dst->tls_cert_path = zend_string_init(
            ZSTR_VAL(src->tls_cert_path), ZSTR_LEN(src->tls_cert_path), 0);
    }

    if (src->tls_key_path) {
        dst->tls_key_path = zend_string_init(
            ZSTR_VAL(src->tls_key_path), ZSTR_LEN(src->tls_key_path), 0);
    }

    if (src->http3_hq_docroot) {
        dst->http3_hq_docroot = zend_string_init(
            ZSTR_VAL(src->http3_hq_docroot), ZSTR_LEN(src->http3_hq_docroot), 0);
    }

    if (src->listener_count > 0) {
        dst->listeners = ecalloc(src->listener_count, sizeof(http_listener_config_t));
        dst->listener_capacity = src->listener_count;
        dst->listener_count    = src->listener_count;
        for (size_t i = 0; i < src->listener_count; i++) {
            dst->listeners[i].type          = src->listeners[i].type;
            dst->listeners[i].port          = src->listeners[i].port;
            dst->listeners[i].tls           = src->listeners[i].tls;
            dst->listeners[i].protocol_mask = src->listeners[i].protocol_mask;

            if (src->listeners[i].host) {
                dst->listeners[i].host = zend_string_init(
                    ZSTR_VAL(src->listeners[i].host),
                    ZSTR_LEN(src->listeners[i].host), 0);
            }
        }
    }
}

/* transfer_obj handler — called by TrueAsync thread transfer machinery.
 *
 * TRANSFER (source thread → persistent transit):
 *   Freeze lazily if the caller never locked the config, then hand out a
 *   persistent shell whose `frozen` field points at our shared snapshot.
 *   The shell's other fields stay zero (pecalloc) so the default release
 *   path doesn't try to free emalloc pointers it doesn't own.
 *
 * LOAD (transit → destination thread):
 *   Create a fresh emalloc object via create_object, populate its scalar/
 *   string fields from the shared snapshot, and keep the shared pointer +
 *   addref so this config can be re-transferred onward cheaply. */
static zend_object *http_server_config_transfer_obj(
    zend_object *object,
    zend_async_thread_transfer_ctx_t *ctx,
    zend_object_transfer_kind_t kind,
    zend_object_transfer_default_fn default_fn)
{
    if (kind == ZEND_OBJECT_TRANSFER) {
        http_server_config_t *src = http_server_config_from_obj(object);

        /* Ensure frozen snapshot exists. Normally built by http_server_config_lock
         * when wrapped in an HttpServer; fall back to lazy freeze for direct
         * cross-thread transfers (e.g. through a ThreadChannel). */
        if (src->frozen == NULL) {
            src->frozen = http_server_shared_config_freeze(src);
            /* Do not set is_locked — the user hasn't asked to lock this config;
             * modifications in the source thread remain allowed, and a later
             * lock will see frozen != NULL and skip re-freezing. */
        }

        zend_object *dst = default_fn(object, ctx, sizeof(http_server_config_t));

        if (UNEXPECTED(dst == NULL)) {
            return NULL;
        }

        /* default_fn pecalloc'd the full wrapper and memcpy'd only the zend_object
         * region. All fields before `std` in the shell are zero. Stash our
         * frozen pointer (refcount++) so LOAD can reconstruct state. */
        http_server_config_t *dst_shell = http_server_config_from_obj(dst);
        http_server_shared_config_addref(src->frozen);
        dst_shell->frozen = src->frozen;

        return dst;
    }

    /* LOAD */
    http_server_config_t *src_shell = http_server_config_from_obj(object);
    http_server_shared_config_t *shared = src_shell->frozen;

    if (UNEXPECTED(shared == NULL)) {
        /* Shouldn't happen — TRANSFER always sets frozen — but guard anyway. */
        return NULL;
    }

    zend_object *dst = default_fn(object, ctx, 0);

    if (UNEXPECTED(dst == NULL)) {
        return NULL;
    }

    http_server_config_t *dst_cfg = http_server_config_from_obj(dst);

    http_server_config_populate_from_shared(dst_cfg, shared);
    http_server_shared_config_addref(shared);
    dst_cfg->frozen    = shared;
    dst_cfg->is_locked = true;  /* LOADed config inherits the locked status */

    return dst;
}

/* {{{ http_server_config_class_register */
void http_server_config_class_register(void)
{
    http_server_config_ce = register_class_TrueAsync_HttpServerConfig();
    http_server_config_ce->create_object = http_server_config_create;

    memcpy(&http_server_config_handlers, &std_object_handlers, sizeof(zend_object_handlers));
    http_server_config_handlers.offset = offsetof(http_server_config_t, std);
    http_server_config_handlers.free_obj = http_server_config_free;
    http_server_config_handlers.clone_obj = NULL;  /* No cloning */
    http_server_config_handlers.transfer_obj = http_server_config_transfer_obj;

    /* Required so cross-thread LOAD (which has no live source object and
     * dispatches by class name) can find our transfer_obj handler. */
    http_server_config_ce->default_object_handlers = &http_server_config_handlers;
}
/* }}} */

/* Internal API for HttpServer to access config */

void http_server_config_lock(zend_object *obj)
{
    http_server_config_t *config = http_server_config_from_obj(obj);

    if (config->is_locked) {
        return;
    }

    /* Build the persistent snapshot that will be shared (refcounted) with any
     * PHP thread this config is transferred to. Keeping it here — rather than
     * at transfer time — means the freeze cost is paid once per config, and
     * transfer becomes a pointer copy + atomic increment. */
    if (config->frozen == NULL) {
        config->frozen = http_server_shared_config_freeze(config);
    }

    config->is_locked = true;
}

bool http_server_config_is_locked(zend_object *obj)
{
    http_server_config_t *config = http_server_config_from_obj(obj);
    return config->is_locked;
}
