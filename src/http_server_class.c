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
#include "zend_interfaces.h"
#include "zend_closures.h"
#include "main/php_network.h"           /* php_socket_t, SOCK_ERR, closesocket, php_socket_errno */
#include "Zend/zend_async_API.h"
#include "Zend/zend_hrtime.h"
#include "Zend/zend_enum.h"
#include "php_http_server.h"
#include "core/http_connection.h"
#include "core/http_connection_internal.h"
#include "core/body_pool.h"
#ifdef HAVE_HTTP_COMPRESSION
# include "compression/http_compression_pool.h"
#endif
#include "core/conn_arena.h"
#include "core/http_protocol_handlers.h"
#include "core/http_protocol_strategy.h"
#include "core/tls_layer.h"
#include "core/reactor_pool.h"
#include "core/worker_inbox.h"
#include "core/worker_registry.h"
#include "core/stats_registry.h"
#include "core/response_wire.h"
#include "core/stream_credit.h"
#include "core/async_plain_event.h"   /* async_coroutine_sleep_ms */
#include "log/http_log.h"
#ifndef PHP_WIN32
#include <dirent.h>
#include <sys/stat.h>
#endif
#include "static/static_handler.h"
#include "static/http_static_cache.h"
#ifdef HAVE_HTTP_SERVER_HTTP3
# include "http3/http3_listener.h"
# include "http3/http3_steer.h"
#endif

/* Backpressure tunables. Hard-cap hysteresis ratio: pause_low = ratio *
 * pause_high. CoDel interval is the RFC 8289 constant — not tunable.
 * CoDel target is read from HttpServerConfig::backpressure_target_ms at
 * start() and can be overridden via env CODEL_TARGET_MS. */
#define BACKPRESSURE_PAUSE_LOW_RATIO   80   /* percent */
#define CODEL_INTERVAL_NS              (100ULL * 1000000ULL)  /* 100 ms */

/* php_network.h supplies sys/socket.h / winsock. The listen socket is
 * owned by the reactor, and closesocket / SOCK_ERR / php_socket_errno are
 * cross-platform shims. */
#include <stdint.h>

/* AF_UNIX listener support needs filesystem headers for the stale-socket
 * probe and teardown unlink. POSIX only — on Windows uv_pipe maps to a
 * named pipe with no filesystem entry to clean up. */
#ifndef PHP_WIN32
# include <sys/un.h>
# include <sys/stat.h>
# include <unistd.h>
# include <netdb.h>
# include <netinet/in.h>
# include <arpa/inet.h>
#endif

/* See http_connection.c for rationale. */
#ifndef MSG_NOSIGNAL
# define MSG_NOSIGNAL 0
#endif

/* Include generated arginfo */
#include "../stubs/HttpServer.php_arginfo.h"

/* Feature-detect kernel-level load-balanced REUSEPORT (h2o-style):
 * Linux has it since 3.9, FreeBSD 12+ as SO_REUSEPORT_LB. Plain
 * SO_REUSEPORT on macOS/older BSD/OpenBSD/NetBSD/Solaris is
 * NOT load-balanced — it has the BSD multicast meaning. On those
 * the workers share a single fd via dup(). */
/* The worker pool shares one pre-bound listen fd (parent binds, each
 * worker dups) wherever the kernel has no load-balanced SO_REUSEPORT to
 * let every worker bind the same port itself. Linux (3.9+) and FreeBSD
 * (SO_REUSEPORT_LB, 12+) have it; everyone else (macOS, other BSD,
 * Solaris) takes the shared-fd default. */
#if defined(__linux__) || defined(SO_REUSEPORT_LB)
# define HTTP_SHARED_FD_DEFAULT false
#else
# define HTTP_SHARED_FD_DEFAULT true
#endif

/* TRUE_ASYNC_SERVER_SHARED_LISTEN_FD=1/0 overrides the default so the
 * shared-fd path can be exercised on Linux CI without a macOS runner.
 * Windows never shares (no POSIX socket dup), so it is always false. */
static bool http_server_use_shared_listen_fd(void)
{
#ifdef PHP_WIN32
    return false;
#else
    const char *env = getenv("TRUE_ASYNC_SERVER_SHARED_LISTEN_FD");
    if (env != NULL && (env[0] == '0' || env[0] == '1')) {
        return env[0] == '1';
    }

    return HTTP_SHARED_FD_DEFAULT;
#endif
}

/* Whether to ask the reactor for SO_REUSEPORT on each listener. This is a
 * kernel capability, NOT the logical inverse of the shared-fd strategy —
 * conflating the two is what broke Windows (issue #82). Three platform camps:
 *   - Linux/FreeBSD: load-balanced REUSEPORT, each worker binds itself.
 *   - macOS/other BSD: no LB REUSEPORT, so the shared-fd dup model instead.
 *   - Windows: NEITHER. Winsock has no SO_REUSEPORT; libuv's uv_tcp_bind()
 *     returns UV_ENOTSUP ("operation not supported on socket") if
 *     UV_TCP_REUSEPORT is set, so it must never be requested. A single
 *     listener (workers=1, the default) then just binds directly.
 * On POSIX the answer is still !use_shared_listen_fd(); Windows is the third
 * case a lone boolean cannot express, hence its own carve-out here. */
static bool http_server_use_reuseport(void)
{
#ifdef PHP_WIN32
    return false;
#else
    return !http_server_use_shared_listen_fd();
#endif
}

/* Max listeners */
#define MAX_LISTENERS 16

/* Listener info. The listen_event owns the socket fd and the libuv handle;
 * we just store the handle plus any per-listener flags. protocol_mask is
 * the per-listener HTTP_PROTO_MASK_* set; carried onto each spawned
 * http_connection_t and consulted by detect_and_assign_protocol. */
typedef struct {
    zend_async_listen_event_t   *listen_event;
    bool                         tls;
    bool                         unlink_on_close;  /* AF_UNIX — unlink le->host on teardown */
    uint32_t                     protocol_mask;
} http_listener_t;

/* An AF_UNIX socket bound once by the pool parent (workers > 1) and shared
 * with every worker thread — AF_UNIX has no SO_REUSEPORT, so N independent
 * binds on one path is impossible; instead one fd is bound and each worker
 * adopts a dup of it. POD: copied by value across thread transfer (the fd
 * integer is valid in every worker thread — threads share one fd table).
 * path is retained so the parent can unlink it once the pool drains. */
typedef struct {
    char path[108];   /* fits sockaddr_un.sun_path (Linux) */
    int  fd;
} http_pool_unix_fd_t;

/* Pre-bound TCP listener fd for the workers-with-no-SO_REUSEPORT path
 * (macOS, Windows). Same model as http_pool_unix_fd_t: parent binds
 * once, each worker dups. host/port are remembered so the worker can
 * match its config entry to the shared fd. */
typedef struct {
    char host[64];
    int  port;
    int  fd;
} http_pool_tcp_fd_t;

/* AF_UNIX bind() fails with EADDRINUSE on a socket file left behind by a
 * crashed previous run. Probe it: a stale socket refuses connect() with
 * ECONNREFUSED and is safe to unlink; a live one accepts the probe, so we
 * leave it and let bind() report the genuine conflict. */
#ifndef PHP_WIN32
static void http_server_unix_unlink_if_stale(const char *path)
{
    struct stat st;
    if (lstat(path, &st) != 0 || !S_ISSOCK(st.st_mode)) {
        return;
    }

    struct sockaddr_un addr;
    size_t path_len = strlen(path);
    if (path_len >= sizeof(addr.sun_path)) {
        return;  /* over-long — let bind() surface the error */
    }

    int probe = socket(AF_UNIX, SOCK_STREAM, 0);
    if (probe < 0) {
        return;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, path, path_len + 1);

    if (connect(probe, (struct sockaddr *)&addr, sizeof(addr)) != 0
        && (errno == ECONNREFUSED || errno == ENOENT)) {
        unlink(path);
    }

    closesocket(probe);
}
#endif

/* Stop, dispose and (AF_UNIX) unlink one listener row. The socket-file
 * unlink must run before dispose(): libuv never removes the path, and
 * dispose() frees the listen_event that owns the path string. */
static void http_server_listener_release(http_listener_t *listener)
{
    zend_async_listen_event_t *le = listener->listen_event;
    if (le == NULL) {
        return;
    }

    le->base.stop(&le->base);

#ifndef PHP_WIN32
    if (listener->unlink_on_close && le->host != NULL) {
        unlink(le->host);
    }
#endif

    le->base.dispose(&le->base);
    listener->listen_event = NULL;
}

/* Server object structure.
 * Field order grouped by alignment to minimise padding. zend_object must
 * remain last per PHP object layout contract.
 *
 * Struct is tagged (not anonymous) so a forward declaration in
 * php_http_server.h can let http_connection carry a typed pointer without
 * seeing the layout. Only http_server_class.c dereferences these fields. */
/*
 * Lifetime separation: the C-state struct (http_server_object) is split
 * from the PHP wrapper (http_server_php). The PHP wrapper holds the
 * zend_object handle that Zend's GC manages; the C-state is refcounted
 * and can outlive the wrapper. A live connection holds one ref on the
 * C-state, so reactor/libuv shutdown drains that fire late callbacks
 * post-wrapper-free still see valid memory. The last release frees the
 * C-state (including the embedded conn arena slab).
 */
struct http_server_object {
    /* Refcount. PHP wrapper holds 1, each live conn holds 1. */
    int32_t                  refcount;

    zval                     config;             /* HttpServerConfig object */
    HashTable                protocol_handlers;  /* Protocol handlers (type -> callback) */
    /* Read-mostly config snapshot. Layout-public via php_http_server.h.
     * Connections cache &server->view at create time so config getters
     * are a single load, no NULL check. Source-of-truth for protocol_mask,
     * write_timeout_s, stream_write_buffer_bytes, drain_epoch_current, h3 knobs. */
    http_server_view_t       view;

    /* TrueAsync structures */
    zend_async_scope_t      *server_scope;
    zend_object             *scope_object;      /* Owned ref on server_scope's zend object.
                                                 * Stored separately because the scope's own
                                                 * pointer (server_scope->scope_object) gets
                                                 * nulled during scope_destroy, which runs in
                                                 * the dtor phase before our http_server_free. */
    zend_async_event_t      *wait_event;        /* Event to block start() */

    /* Statistics — active_requests + total_requests moved into counters slice. */
    size_t                   active_connections;

    /* Slab allocator for live http_connection_t instances. Owns both
     * the chunk memory AND the intrusive doubly-linked alive list
     * (arena.alive_head). Lifetime is tied to the refcounted C-state:
     * each live conn holds one ref on the C-state, so the arena's
     * memory stays valid until the last conn returns its slot.
     * Walked by the per-worker periodic deadline_tick + future
     * broadcast / admission paths. */
    conn_arena_t             conn_arena;

    /* Per-worker deadline watchdog. Single periodic libuv timer that
     * fires every tick_ms milliseconds. The callback walks
     * conn_arena.alive_head and force-closes any conn whose
     * deadline_ns has elapsed. tick_ms = max(250, min(read, write,
     * keepalive) / 2) — set at start(). Replaces the per-conn
     * write_timer arm/stop dance that used to run on every send. */
    zend_async_event_t          *deadline_tick;
    zend_async_event_callback_t *deadline_tick_cb;

    /* Inline listener array */
    http_listener_t          listeners[MAX_LISTENERS];
    size_t                   listener_count;

    /* AF_UNIX sockets pre-bound by the pool parent (workers > 1), shared
     * with every worker. Empty for single-worker servers and non-clone
     * pool parents that have no unix listener. Copied verbatim to each
     * worker by http_server_transfer_obj. */
    http_pool_unix_fd_t      pool_unix_fds[MAX_LISTENERS];
    size_t                   pool_unix_fd_count;
    http_pool_tcp_fd_t       pool_tcp_fds[MAX_LISTENERS];
    size_t                   pool_tcp_fd_count;

    /* Pure-C transport reactor pool. Parent-only, brought up when an H3
     * listener is configured and the opt-in env gate is set; NULL otherwise.
     * Owns no PHP state. */
    reactor_pool_t          *reactor_pool;

    /* This worker clone's request inbox. Non-NULL only on a worker clone
     * running under the reactor-pool gate; the reactor posts parsed requests
     * here and the drain dispatches them on this thread. */
    worker_inbox_t          *worker_inbox;

#ifdef HAVE_HTTP_SERVER_HTTP3
    /* HTTP/3 UDP listeners — parallel to TCP listeners[] because they have
     * different transport semantics (no accept(), no per-connection fd) and
     * are teardown-driven by http3_listener_destroy(). */
    http3_listener_t        *http3_listeners[MAX_LISTENERS];
    size_t                   http3_listener_count;

    /* Reactor-owned H3 listeners. Parent-only, under the gate: one per
     * (reactor x configured udp_h3 listener), spawned ON the reactor thread so
     * its uv socket lives on the right loop. The parent owns these + their
     * thread-clean contexts + a shared SSL_CTX; all torn down with the reactor
     * pool. Each entry remembers which reactor it runs on so teardown can run
     * on that thread. */
    struct { http3_listener_t *listener; int reactor_id; }
                             reactor_h3_listeners[MAX_LISTENERS];
    size_t                   reactor_h3_listener_count;
    http3_reactor_ctx_t     *reactor_h3_ctx;          /* [reactor count] */
    tls_context_t           *reactor_tls_ctx;         /* parent-built shared SSL_CTX */

    /* CID steering groups, one per H3 endpoint. Each groups that endpoint's
     * per-reactor listeners by reactor id so any reactor can forward a stray
     * (migrated) datagram to the owner. Built after the listeners spawn, freed
     * after they tear down. */
    http3_steer_group_t     *reactor_h3_steer[MAX_LISTENERS];
    size_t                   reactor_h3_steer_count;

    /* Pre-rendered "h3=\":<port>\"; ma=86400" string, refreshed
     * at start() when an H3 listener is configured. NULL when H3 is
     * disabled or env-var opt-out is set. Lifetime: refcounted zend_string,
     * released on stop / object destruction. */
    zend_string             *alt_svc_header_value;
#endif

    /* Shared per-server TLS context. Built lazily in start() when any
     * listener has tls=true; NULL otherwise. A single cert/key is used
     * for every TLS listener on this server — SNI multi-cert lives
     * behind tlsext_servername_cb. */
#ifdef HAVE_OPENSSL
    tls_context_t           *tls_ctx;
#endif

    /* Backpressure — hard cap (operator-facing knob). pause_high =
     * max_connections, pause_low = 0.8 * max_connections. Hysteresis
     * keeps listeners from flapping at the boundary. */
    size_t                   pause_high;
    size_t                   pause_low;

    /* Backpressure — adaptive (CoDel, RFC 8289). All units nanoseconds,
     * all state per-worker (shared-nothing, no atomics). Driven by
     * sojourn samples delivered from http_handler_coroutine_entry.
     * codel_target_ns is the threshold: if min sojourn stays above this
     * value for one full interval, listeners pause. Sourced once at
     * start() from HttpServerConfig::setBackpressureTargetMs() (default
     * 0 = off — sojourn misfires on HTTP/2 mux), with env
     * CODEL_TARGET_MS as ops-time override. */
    uint64_t                 codel_target_ns;
    uint64_t                 codel_window_start_ns;
    uint64_t                 codel_min_sojourn_ns;
    uint64_t                 codel_first_above_target_ns;

    /* Telemetry — cluster these so the hot sample aggregator writes
     * within a few cache lines. Counter set is one atomic group: every
     * sample updates sojourn_sum + service_sum + sojourn_samples
     * (possibly sojourn_max). service_samples collapsed — each sample
     * carries both sojourn and service, counts are identical. */
    uint64_t                 sojourn_sum_ns;
    uint64_t                 service_sum_ns;
    uint64_t                 sojourn_max_ns;
    uint64_t                 sojourn_samples;
    uint64_t                 pause_count_total;
    uint64_t                 codel_trips_total;
    uint64_t                 paused_since_ns;     /* 0 while accepting */
    uint64_t                 paused_total_ns;

    /* TLS telemetry. Kept in a separate bucket from the sojourn/service
     * aggregates: handshake cost is once-per-connection, not per
     * request, so mixing it in would contaminate CoDel's signal
     * — kept separate by design. All counters are plain
     * uint64_t because every TLS coroutine is single-threaded on a
     * single worker's event loop — no atomics needed. */
    uint64_t                 tls_handshakes_total;
    uint64_t                 tls_handshake_failures_total;
    uint64_t                 tls_handshake_ns_sum;
    uint64_t                 tls_handshake_ns_count;
    uint64_t                 tls_resumed_total;
    /* tls_bytes_* moved into counters slice for inline access. */
    uint64_t                 tls_ktls_tx_total;   /* handshakes where kTLS TX engaged */
    uint64_t                 tls_ktls_rx_total;   /* handshakes where kTLS RX engaged */

    /* Parser-error telemetry. One bump per
     * RFC-compliant 4xx parse-error response emitted before tearing
     * down the connection. parse_errors_4xx_total == sum of the four
     * per-status counters. Independent from CoDel sojourn samples. */
    uint64_t                 parse_errors_4xx_total;
    uint64_t                 parse_errors_400_total;
    uint64_t                 parse_errors_413_total;
    uint64_t                 parse_errors_414_total;
    uint64_t                 parse_errors_431_total;
    uint64_t                 parse_errors_503_total;

    /* Admission-reject + drain telemetry: requests_shed_total moved to
     * counters slice; drain_epoch_current moved to view (read by every
     * conn create + every commit; readers cache view pointer). */
    uint64_t                 drain_last_fired_ns;

    /* Config cache — nanoseconds, 0 = feature disabled per knob. */
    uint64_t                 max_connection_age_ns;
    uint64_t                 max_connection_age_grace_ns;
    uint64_t                 drain_spread_ns;
    uint64_t                 drain_cooldown_ns;

    /* Drain/connection counters that aren't on the per-request hot path
     * — kept here, not in the inline counters slice. */
    uint64_t                 connections_drained_reactive_total;
    uint64_t                 connections_drained_proactive_total;
    uint64_t                 connections_force_closed_total;
    uint64_t                 drain_events_reactive_total;
    uint64_t                 drain_events_cooldown_blocked_total;

    /* Hot-path counters slice. Layout-public via php_http_server.h so
     * other TUs can bump fields directly via the inline helpers — one
     * load + one inc/add at the call site, no function call, no NULL
     * check. Connections cache counters_live (below) at create time. */
    http_server_counters_t   counters;

    /* Live counter target (issue #5, A2). &counters for a standalone/parent
     * server; the shared stats-slab slot for a pool worker. Everything that is
     * not already reaching through a cached conn->counters pointer goes via
     * this, so the telemetry API can sum each worker's slot. stats_slot is the
     * claimed slab index, -1 when the server owns no slot. */
    http_server_counters_t  *counters_live;
    int                      stats_slot;

    /* Config values (cached). Timeouts are in seconds; 0 = disabled.
     * Unsigned gives non-negative semantics without sentinel confusion.
     * write_timeout_s, stream_write_buffer_bytes, http3_*, protocol_mask,
     * drain_epoch_current moved to view. */
    int                      backlog;            /* passed to listen(2) as int */
    int                      max_connections;    /* 0 = unlimited */
    size_t                   max_inflight_requests; /* 0 = disabled, admission-reject cap */
    uint32_t                 read_timeout_s;
    uint32_t                 keepalive_timeout_s;
    uint32_t                 shutdown_timeout_s;

    http_log_state_t         log_state;

    /* State flags (clustered) */
    bool                     running;
    bool                     stopping;
    bool                     listeners_paused;
    /* Set once start()'s post-wakeup drain has emptied server_scope, so the
     * http_server_free fallback drain is skipped on the normal stop() path
     * (issue #74). */
    bool                     scope_drained;
    /* Set in transfer_obj LOAD when this object was constructed by the
     * built-in worker pool (issue #11) — start() skips re-spawning the
     * pool and runs the standalone event loop. */
    bool                     is_worker_clone;
    /* True only while the parent server is awaiting its child worker
     * pool. stop() consults this to refuse pool-mode shutdown — the
     * parent has no listen events of its own. */
    bool                     in_pool_mode;

    /* Pool-mode worker ctx array (issue #11). pemalloc'd block of N
     * pool_worker_ctx_t entries; each holds an independent persistent
     * shell of the parent HttpServer zval (one transfer per worker —
     * snapshot deep-copy is not safe under concurrent LOAD). Lifetime
     * is the parent server's: alloced in start_pool, freed in
     * http_server_free. NULL outside pool mode. */
    void                    *pool_worker_ctx;
    int                      pool_worker_ctx_count;

    /* The built-in worker pool handle (issue #93 hot reload). Retained on the
     * parent so HttpServer::reload() can drive ThreadPool::reload() — swap the
     * task channel, drain the old worker cohort, and resubmit fresh start()
     * tasks. NULL outside pool mode; owned ref released in start_pool cleanup. */
    zend_async_thread_pool_t *worker_pool;

    /* Hot-reload beacon (issue #93): pemalloc'd http_server_reload_shared_t
     * owned by the pool parent (start_pool alloc/free), pointer fanned out to
     * worker clones through the transfer shells. A worker watches `epoch` from
     * its deadline tick and self-stops when it moves; the parent bumps it and
     * rotates the pool. */
    void                    *reload_shared;
    int                      reload_epoch_seen; /* worker clone: last epoch acted on */
    bool                     reload_in_progress;/* parent: one rotation at a time */
    void                    *pool_await_state;  /* parent: st of the active pool run */

    /* Hot-reload triggers (issue #93), pool parent only. Watcher objects are
     * closed and released by http_server_hot_reload_down; the SIGHUP event is
     * owned by its orchestrator coroutine (the pointer here only lets teardown
     * wake it). */
    zval                     hot_reload_watchers; /* array of Async\FileSystemWatcher */
    void                    *sighup_event;        /* zend_async_signal_event_t* */
    bool                     hot_reload_stopping; /* orchestrators must exit */

    /* Transit sidecar — non-NULL only in the persistent shell created by
     * transfer_obj(TRANSFER). Holds pemalloc-copied closures so the LOAD
     * side can rebuild fcall_t entries in the destination thread's heap.
     * Always NULL in the source thread's emalloc object. */
    void                    *transit_handlers;

    /* Pemalloc'd http_server_transit_static_t side-car (issue #13).
     * Holds pre-addref'd shared mount pointers from TRANSFER; the LOAD
     * path addrefs once more into the worker's emalloc array. NULL
     * outside the worker-pool fan-out. */
    void                    *transit_static_mounts;

    /* Built-in static file mounts (issue #13). Each entry references a
     * locked TrueAsync\StaticHandler PHP object whose embedded
     * descriptor we read on the dispatch fast path. The array is
     * append-only, sized by static_handler_capacity. We hold a strong
     * ref on each handler PHP object via static_handler_objects so the
     * descriptors stay alive while requests are in flight. The mount
     * pointers are read-mostly after start() — no atomics needed. */
    http_static_handler_t  **static_handler_mounts;
    zend_object            **static_handler_objects;
    size_t                   static_handler_count;
    size_t                   static_handler_capacity;

    /* Open file cache (LRU, TTL-bound) — populated lazily on the
     * first http_static_try_serve hit, when at least one StaticHandler
     * has called setOpenFileCache. NULL when no mount opts in.
     * static_cache_resolved disambiguates "still uninitialised" from
     * "intentionally disabled". Freed at server destroy. See
     * include/static/http_static_cache.h. */
    http_static_cache_t     *static_cache;
    uint8_t                  static_cache_resolved;
};

/* PHP wrapper. The std handle is what Zend hands out to userland; the
 * server pointer reaches into the refcounted C-state. */
struct http_server_php {
    http_server_object *server;
    zend_object         std;
};

/* Class entry */
zend_class_entry *http_server_ce;
static zend_object_handlers http_server_handlers;

/* Thread-local current server (for accept callback) */
#ifdef PHP_WIN32
static __declspec(thread) http_server_object *current_server = NULL;
#else
static __thread http_server_object *current_server = NULL;
#endif

static inline struct http_server_php *http_server_php_from_obj(zend_object *obj) {
    return (struct http_server_php *)((char *)(obj) - offsetof(struct http_server_php, std));
}

static inline http_server_object *http_server_from_obj(zend_object *obj) {
    return http_server_php_from_obj(obj)->server;
}
#define Z_HTTP_SERVER_P(zv) http_server_from_obj(Z_OBJ_P(zv))

http_server_object *http_server_object_from_zend(zend_object *obj) {
    return http_server_from_obj(obj);
}

/* Single-thread per worker — no atomics needed. */
static void http_server_state_finalize(http_server_object *server);

void http_server_addref(http_server_object *server) {
    if (server == NULL) return;
    server->refcount++;
}

void http_server_release(http_server_object *server) {
    if (server == NULL) return;

    if (--server->refcount == 0) {
        http_server_state_finalize(server);
        pefree(server, /*persistent*/ 0);
    }
}

static HashTable *http_server_get_gc(zend_object *obj, zval **table, int *n);
static void http_server_accept_callback(
    zend_async_event_t *event,
    zend_async_event_callback_t *callback,
    void *result,
    zend_object *exception);

/* Hot-reload beacon (issue #93): one per pool run, pemalloc'd and owned by the
 * pool parent (start_pool alloc/free); worker clones get the pointer through
 * their transfer shells and watch `epoch` from the deadline tick below. */
typedef struct {
    zend_atomic_int epoch;
} http_server_reload_shared_t;

static void http_server_do_stop(http_server_object *server, const char *reason);
static void http_server_hot_reload_up(http_server_object *server, zval *this_zv,
                                      http_server_config_t *cfg);
static void http_server_hot_reload_down(http_server_object *server);
static void http_server_worker_inbox_retire(http_server_object *server);

/* Runs on a fresh coroutine enqueued by the deadline tick: stop() disposes that
 * very tick timer, which must not happen from inside its own callback. */
static void http_server_reload_stop_entry(void)
{
    http_server_object *server =
        (http_server_object *) ZEND_ASYNC_CURRENT_COROUTINE->extended_data;

    if (UNEXPECTED(server == NULL || !server->running || server->stopping)) {
        return;
    }

    /* Reactor-pool mode: unpublish our inbox and fence the reactors BEFORE the
     * stop — a producer must never post into a dying worker's inbox (#93). */
    http_server_worker_inbox_retire(server);

    server->stopping = true;
    fprintf(stderr, "[true-async-server] worker shutting down (reason=reload, grace=%us)\n",
            server->shutdown_timeout_s);
    fflush(stderr);
    http_server_do_stop(server, "reload");
}

/* ==========================================================================
 * Deadline watchdog — one periodic libuv timer per worker, walks the
 * conn arena's alive list each tick and force-closes conns whose
 * deadline_ns has elapsed. Replaces per-conn / per-await timers.
 * ========================================================================== */

typedef struct {
    zend_async_event_callback_t  base;
    http_server_object          *server;
} http_server_deadline_cb_t;

static void http_server_deadline_cb_dispose(zend_async_event_callback_t *cb,
                                            zend_async_event_t *event)
{
    (void)event;
    efree(cb);
}

static void http_server_deadline_tick_fn(zend_async_event_t *event,
                                         zend_async_event_callback_t *callback,
                                         void *result,
                                         zend_object *exception)
{
    (void)event; (void)result; (void)exception;
    http_server_deadline_cb_t *cb = (http_server_deadline_cb_t *)callback;
    http_server_object *server = cb->server;

    if (UNEXPECTED(server == NULL)) {
        return;
    }

    const uint64_t now = ZEND_ASYNC_NOW();
    http_connection_t *c = server->conn_arena.alive_head;
    while (c != NULL) {
        /* `next` captured before destroy: arena_free unlinks `c`. */
        http_connection_t *next = c->next_conn;

        if (c->deadline_ms != 0 && now >= c->deadline_ms) {
            http_connection_destroy(c);
        }

        c = next;
    }

    /* Hot reload (issue #93): the pool parent bumped the shared epoch — retire
     * this worker from a fresh coroutine, not from this timer's own callback. */
    if (server->is_worker_clone && server->reload_shared != NULL
        && server->running && !server->stopping) {
        http_server_reload_shared_t *shared = server->reload_shared;
        const int epoch = zend_atomic_int_load(&shared->epoch);

        if (UNEXPECTED(epoch != server->reload_epoch_seen)) {
            /* Main scope, not server_scope: the retire coroutine must survive
             * the scope drain it triggers. */
            zend_coroutine_t *co = ZEND_ASYNC_NEW_COROUTINE(ZEND_ASYNC_MAIN_SCOPE);

            if (EXPECTED(co != NULL)) {
                server->reload_epoch_seen = epoch;
                co->internal_entry = http_server_reload_stop_entry;
                co->extended_data  = server;
                ZEND_ASYNC_ENQUEUE_COROUTINE(co);
            }
        }
    }
}

/* tick_ms = max(250, min(read, write, keepalive) / 2). */
static uint32_t http_server_deadline_tick_ms(const http_server_object *server)
{
    uint32_t r = server->read_timeout_s ? server->read_timeout_s * 1000 : UINT32_MAX;
    uint32_t w = server->view.write_timeout_s ? server->view.write_timeout_s * 1000 : UINT32_MAX;
    uint32_t k = server->keepalive_timeout_s ? server->keepalive_timeout_s * 1000 : UINT32_MAX;
    uint32_t m = r;

    if (w < m) m = w;

    if (k < m) m = k;

    if (m == UINT32_MAX) {
        /* All timeouts disabled — still tick at a slow cadence so a
         * broadcast / future graceful-shutdown walker has a heartbeat. */
        m = 120000;
    }

    uint32_t tick = m / 2;

    if (tick < 250) tick = 250;

    /* Pool workers also watch the hot-reload epoch from this tick — cap the
     * cadence so a reload is noticed within a second (issue #93). */
    if (server->is_worker_clone && server->reload_shared != NULL && tick > 1000) {
        tick = 1000;
    }

    return tick;
}

/* Lazy start: armed the first time start() runs. Failure is non-fatal —
 * the server still serves requests, just without idle-conn reaping. */
static void http_server_deadline_tick_start(http_server_object *server)
{
    if (server->deadline_tick != NULL) {
        return;
    }

    const uint32_t tick_ms = http_server_deadline_tick_ms(server);
    zend_async_timer_event_t *t = ZEND_ASYNC_NEW_TIMER_EVENT((zend_ulong)tick_ms, /*periodic*/ true);

    if (UNEXPECTED(t == NULL)) {
        return;
    }
    /* Multishot ensures the event survives across periodic fires —
     * libuv_timer_rearm rejects non-multishot timers. */
    ZEND_ASYNC_TIMER_SET_MULTISHOT(t);

    http_server_deadline_cb_t *cb = (http_server_deadline_cb_t *)
        ZEND_ASYNC_EVENT_CALLBACK_EX(http_server_deadline_tick_fn, sizeof(*cb));

    if (UNEXPECTED(cb == NULL)) {
        t->base.dispose(&t->base);
        return;
    }

    cb->base.dispose = http_server_deadline_cb_dispose;
    cb->server = server;

    if (!t->base.add_callback(&t->base, &cb->base)) {
        efree(cb);
        t->base.dispose(&t->base);
        return;
    }

    server->deadline_tick    = &t->base;
    server->deadline_tick_cb = &cb->base;

    if (!t->base.start(&t->base)) {
        zend_async_callbacks_remove(&t->base, &cb->base);
        t->base.dispose(&t->base);
        server->deadline_tick    = NULL;
        server->deadline_tick_cb = NULL;
    }
}

static void http_server_deadline_tick_stop(http_server_object *server)
{
    if (server->deadline_tick_cb != NULL) {
        http_server_deadline_cb_t *cb = (http_server_deadline_cb_t *)server->deadline_tick_cb;
        cb->server = NULL;  /* defensive: in-flight tick won't deref */
        if (server->deadline_tick != NULL) {
            zend_async_callbacks_remove(server->deadline_tick, server->deadline_tick_cb);
        } else if (server->deadline_tick_cb->dispose != NULL) {
            server->deadline_tick_cb->dispose(server->deadline_tick_cb, NULL);
        }

        server->deadline_tick_cb = NULL;
    }

    if (server->deadline_tick != NULL) {
        if (server->deadline_tick->loop_ref_count > 0) {
            server->deadline_tick->stop(server->deadline_tick);
        }

        server->deadline_tick->dispose(server->deadline_tick);
        server->deadline_tick = NULL;
    }
}

/* Backpressure primitives. Calling stop()/start() on a zend_async listen
 * event toggles its presence in the reactor poll set — no fd is closed,
 * so new SYNs accumulate in the kernel backlog while we are paused and
 * are picked up normally when we resume. Hard cap and CoDel share the
 * same pause/resume plumbing; they differ only in their trigger. */
static void http_server_pause_listeners(http_server_object *server, bool drain_connections);
static void http_server_resume_listeners(http_server_object *server);

/* http_server_on_request_sample / http_server_on_connection_close are
 * declared in php_http_server.h and called directly from
 * http_connection.c — no callback indirection. */

/* Hot-path counter aggregation. Always-inlined so the sample callback
 * emits the three writes + one branch directly at the call site — no
 * function call, no stack frame. Struct layout above clusters these
 * fields (sojourn_sum/max/samples + service_sum) on a small number of
 * cache lines so this whole block writes within L1. */
static zend_always_inline void
http_server_aggregate_sample(http_server_object *s,
                             const uint64_t sojourn_ns,
                             const uint64_t service_ns)
{
    s->sojourn_sum_ns += sojourn_ns;
    s->service_sum_ns += service_ns;
    s->sojourn_samples++;

    if (sojourn_ns > s->sojourn_max_ns) {
        s->sojourn_max_ns = sojourn_ns;
    }
}

/* Pauses accept (REUSEPORT hash excludes this worker) and, if the
 * caller is an overload trigger (drain_connections=true), also bumps
 * the drain epoch so already-accepted connections get a migration
 * signal on their next response. The bool lets future maintenance-
 * pause callers pause without kicking users off. */
static void http_server_pause_listeners(http_server_object *server,
                                        const bool drain_connections)
{
    if (server->listeners_paused) {
        return;
    }

    for (size_t i = 0; i < server->listener_count; i++) {
        zend_async_listen_event_t *le = server->listeners[i].listen_event;

        if (le) {
            le->base.stop(&le->base);
        }
    }

    server->listeners_paused = true;
    server->pause_count_total++;
    server->paused_since_ns = zend_hrtime();

    if (drain_connections) {
        http_server_trigger_drain(server);
    }
}

static void http_server_resume_listeners(http_server_object *server)
{
    if (!server->listeners_paused) {
        return;
    }

    for (size_t i = 0; i < server->listener_count; i++) {
        zend_async_listen_event_t *le = server->listeners[i].listen_event;

        if (le) {
            le->base.start(&le->base);
        }
    }

    if (server->paused_since_ns) {
        server->paused_total_ns += zend_hrtime() - server->paused_since_ns;
        server->paused_since_ns = 0;
    }

    server->listeners_paused = false;
    /* Reset CoDel state so the next window starts clean — otherwise a
     * stale first_above_target deadline could trip pause on the very
     * next sample after resume. */
    server->codel_window_start_ns = 0;
    server->codel_min_sojourn_ns = 0;
    server->codel_first_above_target_ns = 0;
}

/* CoDel sample handler (RFC 8289 simplified for pause/resume rather than
 * per-packet drop scheduling). Track min sojourn in a sliding 100 ms
 * window; if the minimum stays above 5 ms for one full window, the
 * queue is persistent and we pause the listeners. Resume is driven
 * separately by the hard-cap hysteresis (active_connections <= pause_low)
 * — CoDel cannot self-resume because paused listeners produce no new
 * samples. */
void http_server_on_request_sample(http_server_object *server,
                                   const uint64_t sojourn_ns,
                                   const uint64_t service_ns,
                                   const uint64_t now_ns)
{
    if (!server) {
        return;
    }

    /* Aggregate sojourn/service samples for telemetry. total_requests is
     * bumped separately by http_server_count_request — hot-path callers
     * count requests even when stamps are gated off. */
    http_server_aggregate_sample(server, sojourn_ns, service_ns);

    /* Target = 0 disables CoDel entirely. */
    if (server->codel_target_ns == 0) {
        return;
    }

    /* CoDel: track min sojourn in 100ms window. `now_ns` is reused
     * from the caller's already-taken stamp (req->end_ns) — no fresh
     * zend_hrtime on the hot path. */
    const uint64_t now = now_ns;

    if (server->codel_window_start_ns == 0
        || now - server->codel_window_start_ns >= CODEL_INTERVAL_NS) {
        server->codel_window_start_ns = now;
        server->codel_min_sojourn_ns = sojourn_ns;
    } else if (sojourn_ns < server->codel_min_sojourn_ns) {
        server->codel_min_sojourn_ns = sojourn_ns;
    }

    if (server->codel_min_sojourn_ns < server->codel_target_ns) {
        /* Good queue (or burst that drained at least once in the window).
         * Clear the overrun deadline; nothing to do. */
        server->codel_first_above_target_ns = 0;
        return;
    }

    if (server->codel_first_above_target_ns == 0) {
        server->codel_first_above_target_ns = now + CODEL_INTERVAL_NS;
    } else if (now >= server->codel_first_above_target_ns
               && !server->listeners_paused) {
        server->codel_trips_total++;
        /* Overload trigger — also drains existing connections. */
        http_server_pause_listeners(server, /*drain_connections=*/true);
    }
}

/* {{{ TLS telemetry hooks.
 *
 * Everything is in its own bucket — CoDel's signal is strictly request
 * sojourn, and handshake cost / record bytes don't belong in that
 * number. Callers in http_connection.c feed these on:
 *   - handshake_done: measured from TLS coroutine entry to TLS_IO_OK.
 *   - handshake_failed: any TLS_IO_ERROR exit before TLS_ESTABLISHED.
 *   - io: per-loop-iteration deltas (plaintext in/out, ciphertext in/out).
 *
 * All three are safe with server == NULL (tests, unsupervised connections).
 */
void http_server_on_tls_handshake_done(http_server_object *server,
                                       const uint64_t duration_ns,
                                       const bool resumed)
{
    if (server == NULL) {
        return;
    }

    server->tls_handshakes_total++;
    server->tls_handshake_ns_sum   += duration_ns;
    server->tls_handshake_ns_count += 1;

    if (resumed) {
        server->tls_resumed_total++;
    }
}

void http_server_on_tls_handshake_failed(http_server_object *server)
{
    if (server == NULL) {
        return;
    }

    server->tls_handshake_failures_total++;
}

/* http_server_on_tls_io migrated to inline in php_http_server.h —
 * callers pass conn->counters directly. */

void http_server_on_tls_ktls(http_server_object *server,
                             const bool tx_active, const bool rx_active)
{
    if (server == NULL) {
        return;
    }

    if (tx_active) server->tls_ktls_tx_total++;

    if (rx_active) server->tls_ktls_rx_total++;
}
/* }}} */

/* {{{ http_server_on_parse_error
 *
 * Bump per-status + aggregate parse-error counters. Caller (the
 * connection layer) maps the parser's enum to the wire status before
 * calling so the counters here are the same numbers ops will see in
 * dashboards and access logs. Unknown status codes still bump the
 * aggregate so we don't silently miss anything.
 */
void http_server_on_parse_error(http_server_object *server, const int status_code)
{
    if (server == NULL) {
        return;
    }

    server->parse_errors_4xx_total++;
    switch (status_code) {
        case 400: server->parse_errors_400_total++; break;
        case 413: server->parse_errors_413_total++; break;
        case 414: server->parse_errors_414_total++; break;
        case 431: server->parse_errors_431_total++; break;
        case 503: server->parse_errors_503_total++; break;
        default:  /* aggregate-only */ break;
    }
}
/* }}} */

/* {{{ Request-lifetime tracking + admission reject.
 *
 * `active_requests` brackets every dispatched handler coroutine.
 * `http_server_should_shed_request` is the fast-path predicate checked
 * in H1 on_headers_complete and H2 cb_on_begin_headers before any
 * per-request allocation: when the server is paused (CoDel/hard-cap)
 * or the in-flight cap is hit, we short-circuit — H1 with 503
 * Service Unavailable + Retry-After, H2 with RST_STREAM
 * (REFUSED_STREAM), which is retry-safe per RFC 7540 §8.1.4.
 */
/* http_server_on_request_dispatch / _dispose migrated to inline in
 * php_http_server.h — callers pass conn->counters directly. */

bool http_server_should_shed_request(const http_server_object *server)
{
    if (server == NULL) {
        return false;
    }
    /* Operator-facing cap on concurrent handler coroutines. 0 =
     * feature disabled. Crossing the cap is cheap to detect (one load
     * + cmp) and semantically says "event-loop can't handle more,
     * shed right now".
     *
     * We deliberately do NOT key admission on listeners_paused:
     * pause_listeners stops accepting NEW connections, but requests
     * arriving on ALREADY-accepted connections (HTTP/1 keep-alive
     * pipelining, HTTP/2 streams on live sessions) must still be
     * served so the drain cycle completes cleanly — drain
     * writes its Connection: close / GOAWAY on the normal response
     * path. If the real overload signal is needed, the in-flight cap
     * catches it: once max_inflight_requests is hit, those same
     * existing connections also get 503 / REFUSED_STREAM. */
    if (server->max_inflight_requests > 0
        && server->counters_live->active_requests >= server->max_inflight_requests) {
        return true;
    }

    return false;
}

/* http_server_on_request_shed migrated to inline. */
/* }}} */

/* {{{ Drain helpers.
 *
 * Two entry points, one state machine. trigger_drain bumps the epoch
 * with cooldown gating; should_drain_now is the single predicate
 * consulted at every response-commit point (HTTP/1 dispose + HTTP/2
 * stream commit).
 *
 * Pull-model rationale: no server-side list of connections, no O(N)
 * walk on trigger. Each connection discovers the new epoch lazily on
 * its next commit, computes its own spread-jitter offset, and drains
 * itself when the offset elapses. Idle keep-alive connections that
 * never make another request get reaped by keepalive_timeout_s — not
 * our problem (they're not contributing to overload anyway).
 */

/* Alt-Svc value pre-rendered at start(). NULL when no H3
 * listener is up or PHP_HTTP3_DISABLE_ALT_SVC is set. Returned
 * pointer is owned by the server object; do not release. */
zend_string *http_server_get_alt_svc_value(const http_server_object *server)
{
#ifdef HAVE_HTTP_SERVER_HTTP3
    return server != NULL ? server->alt_svc_header_value : NULL;
#else
    (void)server;
    return NULL;
#endif
}

uint64_t http_server_get_max_connection_age_ns(const http_server_object *server)
{
    return server != NULL ? server->max_connection_age_ns : 0;
}

int http_server_get_max_connections(const http_server_object *server)
{
    return server != NULL ? server->max_connections : 0;
}

/* Process-wide fallback counters / view. Used for unsupervised
 * connections (server == NULL) and after http_server_free clears
 * conn->server back-pointers. The dummy counters is write-mostly garbage
 * — nothing reads its fields. The default view holds the same defaults
 * a freshly object_new'd server exposes pre-start(). */
http_server_counters_t http_server_counters_dummy = {0};
const http_server_view_t http_server_view_default = {
    .protocol_mask = HTTP_PROTO_MASK_ALL,
    .write_timeout_s = 0,
    .stream_write_buffer_bytes = 0,
    .drain_epoch_current = 0,
    .http3_idle_timeout_ms = 0,
    .http3_stream_window_bytes = 0,
    .http3_max_concurrent_streams = 0,
    .http3_peer_connection_budget = 0,
    .http3_socket_buffer_bytes = 0,
    .tls_buffer_bytes = 0,
    .http3_alt_svc_enabled = true,
    .http3_pacing = false,
    .request_scope = true,
};

/* Counters / view accessors. Always return a non-NULL pointer; callers
 * cache once at create time. */
http_server_counters_t *http_server_counters(http_server_object *server)
{
    return server != NULL ? server->counters_live : &http_server_counters_dummy;
}

const http_server_view_t *http_server_view(const http_server_object *server)
{
    return server != NULL ? &server->view : &http_server_view_default;
}

size_t http_static_handler_count(const http_server_object *server)
{
    return server != NULL ? server->static_handler_count : 0;
}

const http_static_handler_t *
http_static_handler_get(const http_server_object *server, size_t index)
{
    if (server == NULL || index >= server->static_handler_count) {
        return NULL;
    }

    return server->static_handler_mounts[index];
}

const http_static_handler_t *const *
http_static_handler_mounts(const http_server_object *server)
{
    if (server == NULL || server->static_handler_count == 0) {
        return NULL;
    }

    return (const http_static_handler_t *const *)server->static_handler_mounts;
}

/* Open-file cache accessor. The cache instance is per-server (per-worker
 * after worker-pool transfer — no cross-worker sharing, no locking).
 *
 * Effective settings derive from the registered StaticHandlers:
 *   max_entries = max(mount->cache_max_entries) over enabled mounts
 *   ttl_seconds = min(mount->cache_ttl_seconds) over enabled mounts
 *
 * Disabled (returns NULL) when no mount opts in via
 * StaticHandler::setOpenFileCache(). Mount config is append-only after
 * start() so the merge runs once and is cached via static_cache_resolved
 * — subsequent calls hit the EXPECTED branch. */
http_static_cache_t *http_static_cache_acquire(http_server_object *server)
{
    if (UNEXPECTED(server == NULL)) {
        return NULL;
    }

    if (EXPECTED(server->static_cache != NULL)) {
        return server->static_cache;
    }

    if (server->static_cache_resolved) {
        return NULL;
    }

    int32_t max_entries = 0;
    int32_t ttl_seconds = 0;
    const size_t mount_count = http_static_handler_count(server);
    for (size_t i = 0; i < mount_count; i++) {
        const http_static_handler_t *m = http_static_handler_get(server, i);

        if (m == NULL || m->cache_max_entries <= 0 || m->cache_ttl_seconds <= 0) {
            continue;
        }

        if (m->cache_max_entries > max_entries) {
            max_entries = m->cache_max_entries;
        }

        if (ttl_seconds == 0 || m->cache_ttl_seconds < ttl_seconds) {
            ttl_seconds = m->cache_ttl_seconds;
        }
    }

    server->static_cache_resolved = 1;

    if (max_entries > 0 && ttl_seconds > 0) {
        server->static_cache = http_static_cache_create(
            (size_t) max_entries, (time_t) ttl_seconds);
    }

    return server->static_cache;
}

HashTable *http_server_get_protocol_handlers(http_server_object *server)
{
    return server != NULL ? &server->protocol_handlers : NULL;
}

zend_async_scope_t *http_server_get_scope(http_server_object *server)
{
    return server != NULL ? server->server_scope : NULL;
}

http_server_config_t *http_server_get_config(http_server_object *server)
{
    if (server == NULL) return NULL;

    if (Z_TYPE(server->config) != IS_OBJECT) return NULL;
    return http_server_config_from_obj(Z_OBJ(server->config));
}

http_log_state_t *http_server_get_log_state(http_server_object *server)
{
    return server != NULL ? &server->log_state : &http_log_state_default;
}

/* Translate the config's log sinks (or the log_severity/log_stream sugar) into
 * http_log_sink_spec_t and activate the logger. Specs are validated at
 * setLogSinks() time, so reads here are unchecked; each sink's transport and
 * formatter come from the sink-type / formatter registry (http_log.c). opened[]
 * holds the per-sink stream ref only until start_sinks takes its own. */
static void http_server_start_logging(http_server_object *server,
                                      http_server_config_t *cfg)
{
    if (cfg == NULL) {
        return;
    }

    http_log_sink_spec_t specs[HTTP_LOG_MAX_SINKS];
    zval                 opened[HTTP_LOG_MAX_SINKS];
    int                  n = 0;

    for (int i = 0; i < HTTP_LOG_MAX_SINKS; i++) {
        ZVAL_UNDEF(&opened[i]);
    }

    if (Z_TYPE(cfg->log_sinks) == IS_ARRAY
        && zend_hash_num_elements(Z_ARRVAL(cfg->log_sinks)) > 0) {

        zval *elem;
        ZEND_HASH_FOREACH_VAL(Z_ARRVAL(cfg->log_sinks), elem) {
            if (n >= HTTP_LOG_MAX_SINKS) {
                break;
            }

            HashTable *spec  = Z_ARRVAL_P(elem);
            zval      *ztype = zend_hash_str_find(spec, "type", sizeof("type") - 1);

            const http_log_sink_type_t *type =
                http_log_sink_type_by_name(Z_STRVAL_P(ztype), Z_STRLEN_P(ztype));
            http_log_write_mode_t mode = HTTP_LOG_WRITE_STREAM;

            if (type == NULL) {
                continue;
            }

            if (type->php_delivery) {
                zval *zlvl_p = zend_hash_str_find(spec, "level", sizeof("level") - 1);
                zval *zcat_p = zend_hash_str_find(spec, "category", sizeof("category") - 1);

                memset(&specs[n], 0, sizeof specs[n]);
                specs[n].level         = (http_log_severity_t)
                    Z_LVAL_P(zend_enum_fetch_case_value(Z_OBJ_P(zlvl_p)));
                specs[n].category_mask = zcat_p != NULL
                    ? http_log_category_mask(Z_STRVAL_P(zcat_p), Z_STRLEN_P(zcat_p))
                    : HTTP_LOG_CAT_APP;
                specs[n].php_cb        = zend_hash_str_find(spec, "callback",
                                                            sizeof("callback") - 1);
                n++;
                continue;
            }

            if (!type->open(spec, &opened[n], &mode)) {
                /* Unreachable target, or a parent-opened 'stream' resource
                 * that cannot cross into this worker thread (use 'file'). */
                fprintf(stderr,
                        "http_server: log sink type '%s' skipped (open failed)\n",
                        Z_STRVAL_P(ztype));
                continue;
            }

            const http_log_formatter_def_t *fdef = type->pinned_formatter;

            if (fdef == NULL) {
                zval *zfmt = zend_hash_str_find(spec, "format", sizeof("format") - 1);

                if (zfmt != NULL) {
                    fdef = http_log_formatter_by_name(Z_STRVAL_P(zfmt),
                                                      Z_STRLEN_P(zfmt));
                }
                if (fdef == NULL) {
                    fdef = http_log_formatter_by_name("plain", sizeof("plain") - 1);
                }
            }

            zval *zlvl    = zend_hash_str_find(spec, "level", sizeof("level") - 1);
            zval *backing = zend_enum_fetch_case_value(Z_OBJ_P(zlvl));
            zval *zcat    = zend_hash_str_find(spec, "category", sizeof("category") - 1);

            specs[n].category_mask = zcat != NULL
                ? http_log_category_mask(Z_STRVAL_P(zcat), Z_STRLEN_P(zcat))
                : HTTP_LOG_CAT_APP;
            specs[n].level        = (http_log_severity_t)Z_LVAL_P(backing);
            specs[n].formatter    = fdef != NULL ? fdef->fn : http_log_format_plain;
            specs[n].formatter_ud = (fdef != NULL && fdef->make_ud != NULL)
                                  ? fdef->make_ud(spec, &opened[n]) : NULL;
            specs[n].formatter_ud_free = fdef != NULL ? fdef->free_ud : NULL;
            specs[n].stream_zv    = &opened[n];
            specs[n].php_cb       = NULL;
            specs[n].write_mode   = mode;
            n++;
        } ZEND_HASH_FOREACH_END();

    } else if (cfg->log_severity != 0 && Z_TYPE(cfg->log_stream) != IS_UNDEF) {
        specs[0].level             = (http_log_severity_t)cfg->log_severity;
        specs[0].category_mask     = HTTP_LOG_CAT_APP;
        specs[0].formatter         = http_log_format_plain;
        specs[0].formatter_ud      = NULL;
        specs[0].formatter_ud_free = NULL;
        specs[0].stream_zv         = &cfg->log_stream;
        specs[0].php_cb            = NULL;
        specs[0].write_mode        = HTTP_LOG_WRITE_STREAM;
        n = 1;
    }

    http_log_server_start_sinks(&server->log_state, specs, n);

    /* The access record's duration comes from the request stamps that CoDel/
     * telemetry normally gate; an access sink is a third consumer. */
    if (server->log_state.has_access) {
        server->view.sample_stamps_enabled = true;
    }

    for (int i = 0; i < HTTP_LOG_MAX_SINKS; i++) {
        if (Z_TYPE(opened[i]) != IS_UNDEF) {
            zval_ptr_dtor(&opened[i]);
        }
    }
}

void http_server_trigger_drain(http_server_object *server)
{
    if (server == NULL) {
        return;
    }
    /* Cooldown window is in seconds (default 10s); drain_last_fired_ns
     * was stamped with the same coarse clock — apples-to-apples. Saves
     * one vdso_clock_gettime per CoDel trip. */
    const uint64_t now = http_now_coarse_ns();

    /* Cooldown: prevent oscillation when CoDel / hard-cap flap. The
     * blocked counter stays observable so operators can tune down a
     * too-aggressive cooldown that's swallowing legitimate triggers. */
    if (server->drain_cooldown_ns > 0
        && server->drain_last_fired_ns != 0
        && now - server->drain_last_fired_ns < server->drain_cooldown_ns) {
        server->drain_events_cooldown_blocked_total++;
        return;
    }

    server->view.drain_epoch_current++;
    server->drain_last_fired_ns = now;
    server->drain_events_reactive_total++;
}

/* Generic pull-model drain decision. Takes state by value, returns
 * updated state in a struct so callers whose flags live in bit-fields
 * (cannot have addresses taken) can write the updated values back
 * field-by-field. The proactive + reactive logic is identical to the
 * original should_drain_now — just unwound from struct access. */
http_server_drain_eval_t http_server_drain_evaluate(http_server_object *server,
                                                    bool drain_pending,
                                                    uint64_t drain_not_before_ns,
                                                    uint64_t drain_epoch_seen,
                                                    uint64_t now_ns)
{
    http_server_drain_eval_t r = {
        .should_drain        = false,
        .drain_pending       = drain_pending,
        .drain_not_before_ns = drain_not_before_ns,
        .drain_epoch_seen    = drain_epoch_seen,
    };

    if (server == NULL) {
        return r;
    }
    /* now_ns reused from caller's stamp (req->end_ns at dispose) —
     * skips a fresh zend_hrtime on the hot path. */
    const uint64_t now = now_ns;

    if (!r.drain_pending
        && server->max_connection_age_ns > 0
        && r.drain_not_before_ns != UINT64_MAX
        && now >= r.drain_not_before_ns) {
        r.drain_pending = true;
        server->connections_drained_proactive_total++;
    }

    if (r.drain_epoch_seen < server->view.drain_epoch_current) {
        r.drain_epoch_seen = server->view.drain_epoch_current;

        if (!r.drain_pending) {
            r.drain_pending       = true;
            r.drain_not_before_ns = now;   /* immediate */
            server->connections_drained_reactive_total++;
        }
    }

    r.should_drain = r.drain_pending
        && r.drain_not_before_ns != UINT64_MAX
        && now >= r.drain_not_before_ns;
    return r;
}

bool http_server_should_drain_now(http_server_object *server,
                                  http_connection_t *conn,
                                  uint64_t now_ns)
{
    if (conn == NULL) {
        return false;
    }

    const http_server_drain_eval_t r = http_server_drain_evaluate(server,
        conn->drain_pending != 0,
        conn->drain_not_before_ns,
        conn->drain_epoch_seen,
        now_ns);
    conn->drain_pending       = r.drain_pending;
    conn->drain_not_before_ns = r.drain_not_before_ns;
    conn->drain_epoch_seen    = r.drain_epoch_seen;
    return r.should_drain;
}
/* }}} */

/* Intrusive connection-list maintenance. Prepend on register, walk + unlink
 * on unregister. O(N) unregister is fine — N is small in practice (matches
 * active_connections counter, which is bounded by max_connections). The
 * list exists solely so http_server_free can clear conn->server back-
 * pointers before the server struct is freed. */
/* Public arena accessor — http_connection.c uses it to allocate /
 * free conn slots without seeing the http_server_object internals. */
conn_arena_t *http_server_arena(http_server_object *server)
{
    return &server->conn_arena;
}

/* Bind a freshly-allocated conn to its owning server's hot-path
 * slices. Called by http_connection_spawn after http_connection_create
 * has obtained a slot from the arena. */
void http_server_bind_connection(http_server_object *server,
                                 struct _http_connection_t *conn)
{
    if (server == NULL || conn == NULL) {
        return;
    }

    http_connection_t *real = (http_connection_t *)conn;
    real->counters  = server->counters_live;
    real->view      = &server->view;
    real->log_state = &server->log_state;
    /* Live config pointer for hot-path readers (compression, future
     * inline accessors). Same NULL-on-server-free discipline as the
     * other cached pointers. */
    if (Z_TYPE(server->config) == IS_OBJECT) {
        real->config = http_server_config_from_obj(Z_OBJ(server->config));
    } else {
        real->config = NULL;
    }
}

/* Connection close hook. Drives active_connections-- and the hysteresis
 * resume trigger. Timings are not reported here — each request already
 * reported its full timing via http_server_on_request_sample. */
void http_server_on_connection_close(http_server_object *server)
{
    if (!server) {
        return;
    }

    if (server->active_connections > 0) {
        server->active_connections--;
    }

    if (server->listeners_paused
        && server->active_connections <= server->pause_low) {
        http_server_resume_listeners(server);
    }
}

/* {{{ proto HttpServer::__construct(HttpServerConfig $config) */
ZEND_METHOD(TrueAsync_HttpServer, __construct)
{
    (void)return_value;
    zval *config_zval;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_OBJECT_OF_CLASS(config_zval, http_server_config_ce)
    ZEND_PARSE_PARAMETERS_END();

    http_server_object *server = Z_HTTP_SERVER_P(ZEND_THIS);

    /* Store config reference */
    ZVAL_COPY(&server->config, config_zval);

    /* Lock config to prevent modifications after server creation */
    http_server_config_lock(Z_OBJ_P(config_zval));
}
/* }}} */

/* {{{ proto HttpServer::addHttpHandler(callable $handler): static */
ZEND_METHOD(TrueAsync_HttpServer, addHttpHandler)
{
    http_server_object *server = Z_HTTP_SERVER_P(ZEND_THIS);

    if (server->running) {
        zend_throw_exception(http_server_runtime_exception_ce,
            "Cannot add handler while server is running", 0);
        return;
    }

    http_protocol_add_handler_internal(
        INTERNAL_FUNCTION_PARAM_PASSTHRU,
        &server->protocol_handlers,
        HTTP_PROTOCOL_HTTP1,
        Z_OBJ_P(ZEND_THIS)
    );
    /* addHttpHandler registers a handler for plain HTTP requests.
     * Historically this serves both HTTP/1 and HTTP/2 on the same port
     * (curl --http1.1 and curl --http2 both end up in the same PHP
     * callback); h2-only deployments use addHttp2Handler exclusively. */
    server->view.protocol_mask |= http_protocol_registration_mask(HTTP_PROTOCOL_HTTP1);
}
/* }}} */

/* {{{ proto HttpServer::addStaticHandler(StaticHandler $handler): static */
ZEND_METHOD(TrueAsync_HttpServer, addStaticHandler)
{
    zval *handler_zv = NULL;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_OBJECT_OF_CLASS(handler_zv, http_static_handler_ce)
    ZEND_PARSE_PARAMETERS_END();

    http_server_object *server = Z_HTTP_SERVER_P(ZEND_THIS);

    if (server->running) {
        zend_throw_exception(http_server_runtime_exception_ce,
            "Cannot add static handler while server is running", 0);
        return;
    }

    zend_object *handler_obj = Z_OBJ_P(handler_zv);
    http_static_handler_t *mount = http_static_handler_from_obj(handler_obj);

    if (UNEXPECTED(mount == NULL || mount->url_prefix == NULL)) {
        zend_throw_exception(http_server_invalid_argument_exception_ce,
            "StaticHandler is not initialised", 0);
        return;
    }

    if (server->static_handler_count >= server->static_handler_capacity) {
        const size_t new_cap = server->static_handler_capacity == 0
            ? 4
            : server->static_handler_capacity * 2;
        server->static_handler_mounts = server->static_handler_mounts
            ? erealloc(server->static_handler_mounts,
                       sizeof(http_static_handler_t *) * new_cap)
            : emalloc(sizeof(http_static_handler_t *) * new_cap);
        server->static_handler_objects = server->static_handler_objects
            ? erealloc(server->static_handler_objects,
                       sizeof(zend_object *) * new_cap)
            : emalloc(sizeof(zend_object *) * new_cap);
        server->static_handler_capacity = new_cap;
    }

    /* Lock the draft, then freeze it into a refcounted persistent
     * shared snapshot. The server stores the snapshot pointer; worker-
     * pool TRANSFER then becomes pointer-copy + addref (issue #13). The
     * userland StaticHandler PHP object is pinned solely so callers can
     * keep a handle without invalidating the snapshot it produced. */
    http_static_handler_lock(mount);

    http_static_handler_t *frozen = http_static_handler_freeze(mount);

    if (UNEXPECTED(frozen == NULL)) {
        zend_throw_exception(http_server_runtime_exception_ce,
            "StaticHandler freeze failed (out of memory)", 0);
        return;
    }

    GC_ADDREF(handler_obj);
    server->static_handler_objects[server->static_handler_count] = handler_obj;
    server->static_handler_mounts[server->static_handler_count]  = frozen;
    server->static_handler_count++;

    /* Mark H1 + H2 as protocols this server speaks — symmetric with
     * the addHttpHandler convention. With the H2 static fast-path
     * landed (issue #13 step 2), a static-only deployment now serves
     * the same mount uniformly over both versions on the same port.
     * (H3 lands in PR #3.) start() preflight uses static_handler_count
     * separately. */
    server->view.protocol_mask |= HTTP_PROTO_MASK_HTTP1 | HTTP_PROTO_MASK_HTTP2;

    RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
}
/* }}} */

/* {{{ proto HttpServer::addWebSocketHandler(callable $handler): static */
ZEND_METHOD(TrueAsync_HttpServer, addWebSocketHandler)
{
    http_server_object *server = Z_HTTP_SERVER_P(ZEND_THIS);

    if (server->running) {
        zend_throw_exception(http_server_runtime_exception_ce,
            "Cannot add handler while server is running", 0);
        return;
    }

    /* Store handler - will be used when WebSocket support is implemented */
    http_protocol_add_handler_internal(
        INTERNAL_FUNCTION_PARAM_PASSTHRU,
        &server->protocol_handlers,
        HTTP_PROTOCOL_WEBSOCKET,
        Z_OBJ_P(ZEND_THIS)
    );
    server->view.protocol_mask |= http_protocol_registration_mask(HTTP_PROTOCOL_WEBSOCKET);
}
/* }}} */

/* {{{ proto HttpServer::addHttp2Handler(callable $handler): static */
ZEND_METHOD(TrueAsync_HttpServer, addHttp2Handler)
{
    http_server_object *server = Z_HTTP_SERVER_P(ZEND_THIS);

    if (server->running) {
        zend_throw_exception(http_server_runtime_exception_ce,
            "Cannot add handler while server is running", 0);
        return;
    }

    /* Store handler - will be used when HTTP/2 support is implemented */
    http_protocol_add_handler_internal(
        INTERNAL_FUNCTION_PARAM_PASSTHRU,
        &server->protocol_handlers,
        HTTP_PROTOCOL_HTTP2,
        Z_OBJ_P(ZEND_THIS)
    );
    server->view.protocol_mask |= http_protocol_registration_mask(HTTP_PROTOCOL_HTTP2);
}
/* }}} */

/* {{{ proto HttpServer::addGrpcHandler(callable $handler): static */
ZEND_METHOD(TrueAsync_HttpServer, addGrpcHandler)
{
    http_server_object *server = Z_HTTP_SERVER_P(ZEND_THIS);

    if (server->running) {
        zend_throw_exception(http_server_runtime_exception_ce,
            "Cannot add handler while server is running", 0);
        return;
    }

    http_protocol_add_handler_internal(
        INTERNAL_FUNCTION_PARAM_PASSTHRU,
        &server->protocol_handlers,
        HTTP_PROTOCOL_GRPC,
        Z_OBJ_P(ZEND_THIS)
    );

    server->view.protocol_mask |= http_protocol_registration_mask(HTTP_PROTOCOL_GRPC);
}
/* }}} */

/* Wait event handlers — used to block start() until stop() is called.
 * The signatures are fixed by zend_async's event-vtable contract.
 *
 * start/stop must participate in TrueAsync's active_event_count: the
 * scheduler's deadlock detector fires when (a) a coroutine is parked,
 * (b) libuv has zero active handles, and (c) active_event_count == 0.
 * Under heavy QUIC load, libuv's recv multishot can transiently report
 * zero ready handles between batches; if our park event isn't counted,
 * the detector sees count==0 and aborts the process with
 * Async\DeadlockError even though new datagrams are about to arrive.
 *
 * Pattern mirrors libuv_trigger_event_start/stop (which counts loop_ref).
 * loop_ref_count guards against re-entry from add_callback paths that
 * may call start a second time before stop. */
static bool server_wait_event_start(zend_async_event_t *event)
{
    if (UNEXPECTED(ZEND_ASYNC_EVENT_IS_CLOSED(event))) {
        return true;
    }

    if (event->loop_ref_count > 0) {
        event->loop_ref_count++;
        return true;
    }

    event->loop_ref_count = 1;
    ZEND_ASYNC_INCREASE_EVENT_COUNT(event);
    return true;
}

static bool server_wait_event_stop(zend_async_event_t *event)
{
    if (event->loop_ref_count == 0) {
        return true;
    }

    if (event->loop_ref_count > 1) {
        event->loop_ref_count--;
        return true;
    }

    event->loop_ref_count = 0;
    ZEND_ASYNC_DECREASE_EVENT_COUNT(event);
    return true;
}

static bool server_wait_event_add_callback(zend_async_event_t *event, zend_async_event_callback_t *callback) {
    return zend_async_callbacks_push(event, callback);
}

static bool server_wait_event_del_callback(zend_async_event_t *event, zend_async_event_callback_t *callback) {
    return zend_async_callbacks_remove(event, callback);
}

static bool server_wait_event_replay(zend_async_event_t *event, zend_async_event_callback_t *callback, zval *result, zend_object **exception) {
    (void)event; (void)callback; (void)result; (void)exception;
    return false;
}

static zend_string *server_wait_event_info(zend_async_event_t *event) {
    (void)event;
    return zend_string_init("HttpServer waiting", sizeof("HttpServer waiting") - 1, 0);
}

static bool server_wait_event_dispose(zend_async_event_t *event) {
    if (ZEND_ASYNC_EVENT_REFCOUNT(event) > 1) {
        ZEND_ASYNC_EVENT_DEL_REF(event);
        return true;
    }

    if (ZEND_ASYNC_EVENT_REFCOUNT(event) == 1) {
        ZEND_ASYNC_EVENT_DEL_REF(event);
    }

    zend_async_callbacks_free(event);
    efree(event);
    return true;
}

/* Create wait event for server blocking */
static zend_async_event_t *create_server_wait_event(void)
{
    zend_async_event_t *event = ecalloc(1, sizeof(zend_async_event_t));
    event->ref_count = 1;
    event->start = server_wait_event_start;
    event->stop = server_wait_event_stop;
    event->add_callback = server_wait_event_add_callback;
    event->del_callback = server_wait_event_del_callback;
    event->replay = server_wait_event_replay;
    event->info = server_wait_event_info;
    event->dispose = server_wait_event_dispose;
    return event;
}

/* Build an Async\Timeout awaitable for `ms` — the cancellation token
 * Scope::awaitCompletion() expects. Returns false (out left UNDEF) on failure. */
static bool http_server_make_timeout(const zend_ulong ms, zval *out)
{
    zval fname, arg;
    ZVAL_STRINGL(&fname, "Async\\timeout", sizeof("Async\\timeout") - 1);
    ZVAL_LONG(&arg, (zend_long) ms);
    ZVAL_UNDEF(out);

    const bool ok = call_user_function(NULL, NULL, &fname, out, 1, &arg) == SUCCESS
                 && EG(exception) == NULL
                 && Z_TYPE_P(out) == IS_OBJECT;

    zval_ptr_dtor(&fname);

    if (false == ok) {
        zval_ptr_dtor(out);
        ZVAL_UNDEF(out);

        if (EG(exception)) {
            zend_clear_exception();
        }
    }

    return ok;
}

/* Async\Scope::isFinished() on the live scope object. */
static bool http_server_scope_is_finished(zend_object *scope_object)
{
    zval retval;
    ZVAL_UNDEF(&retval);
    zend_call_method_with_0_params(scope_object, scope_object->ce, NULL, "isFinished", &retval);

    const bool finished = (Z_TYPE(retval) == IS_TRUE);
    zval_ptr_dtor(&retval);
    return finished;
}

/* Call a no-arg, void Scope method, swallowing any exception — a cancellation
 * thrown during shutdown is expected, not an error. */
static void http_server_scope_call(zend_object *scope_object, const char *method)
{
    zval retval;
    ZVAL_UNDEF(&retval);
    zend_call_method_with_0_params(scope_object, scope_object->ce, NULL, method, &retval);
    zval_ptr_dtor(&retval);

    if (EG(exception)) {
        zend_clear_exception();
    }
}

/* Graceful-shutdown drain (issue #74): empty server_scope before it is
 * disposed. Runs on the start() coroutine after stop() wakes it — not inside
 * stop(), which may be called from a handler that lives in server_scope. Uses
 * the Scope's own API (awaitCompletion within the grace window, then cancel +
 * awaitAfterCancellation); these own the scope lifetime, so nothing dangles. */
static void http_server_drain_scope(http_server_object *server)
{
    if (server->scope_object == NULL || server->server_scope == NULL || server->scope_drained) {
        return;
    }

    /* Need a real coroutine to suspend on. In scheduler/teardown context we
     * cannot await — leave scope_drained unset so a later call may retry. */
    if (ZEND_ASYNC_CURRENT_COROUTINE == NULL || ZEND_ASYNC_IS_SCHEDULER_CONTEXT
        || false == ZEND_ASYNC_ON) {
        return;
    }

    zend_object *scope_object = server->scope_object;
    GC_ADDREF(scope_object); /* hold the Scope object across the method calls */

    /* Phase 1: let handlers finish on their own, bounded by the grace window. */
    const zend_ulong grace_ms = (zend_ulong) server->shutdown_timeout_s * 1000u;

    if (grace_ms > 0 && false == http_server_scope_is_finished(scope_object)) {
        zval timeout;

        if (http_server_make_timeout(grace_ms, &timeout)) {
            zval retval;
            ZVAL_UNDEF(&retval);
            zend_call_method_with_1_params(scope_object, scope_object->ce, NULL,
                                           "awaitCompletion", &retval, &timeout);
            zval_ptr_dtor(&retval);
            zval_ptr_dtor(&timeout);

            if (EG(exception)) {
                zend_clear_exception(); /* grace expired, or completed during the wait */
            }
        }
    }

    /* Phase 2: cancel whatever is still parked, then await the cancellation.
     * server_scope is non-dispose-safely (see start()), so cancel() hard-
     * terminates the handlers rather than zombifying them. */
    if (false == http_server_scope_is_finished(scope_object)) {
        http_server_scope_call(scope_object, "cancel");
        http_server_scope_call(scope_object, "awaitAfterCancellation");
    }

    server->scope_drained = true;
    OBJ_RELEASE(scope_object);
}

/* Accept callback — fired once per accepted connection by the reactor.
 * `result` points to the accepted client socket fd (zend_socket_t) the
 * reactor extracted from libuv; `exception` is set on accept failures.
 * No manual accept() loop here — libuv's uv_listen drains the backlog
 * and notifies us per connection. */
static void http_server_accept_callback(
    zend_async_event_t *event,
    zend_async_event_callback_t *callback,
    void *result,
    zend_object *exception)
{
    (void)callback;

    http_server_object *server = current_server;

    if (!server || server->stopping) {
        return;
    }

    if (UNEXPECTED(exception != NULL)) {
        /* Accept error — reactor already attached the exception; drop it
         * here to avoid stopping the loop for a transient per-connection
         * failure. */
        return;
    }

    if (UNEXPECTED(result == NULL)) {
        return;
    }

    php_socket_t client_fd = *(const zend_socket_t *)result;

    if (client_fd == SOCK_ERR) {
        return;
    }

    /* Hard-cap safety net. Under normal operation listeners_paused will
     * already be true here (either CoDel or hysteresis flipped it), so
     * we rarely take this path. But the pause transition can race with
     * an accept already in flight — reject such strays cheaply with
     * 503 rather than spawning a connection we can't serve. */
    if (server->max_connections > 0 &&
        server->active_connections >= (size_t)server->max_connections) {
        const char *response = "HTTP/1.1 503 Service Unavailable\r\n"
                               "Content-Length: 19\r\n"
                               "Connection: close\r\n\r\n"
                               "Service Unavailable";
        send(client_fd, response, (int)strlen(response), MSG_NOSIGNAL);
        closesocket(client_fd);
        /* Make sure listeners are paused so we stop accepting new ones. */
        if (!server->listeners_paused) {
            /* Race fallback — still counts as overload, drain existing. */
            http_server_pause_listeners(server, /*drain_connections=*/true);
        }

        return;
    }

    /* Pre-accept sanity check: at least one HTTP-family handler must be
     * registered, OR at least one static mount (issue #13). Actual
     * handler selection happens later, after protocol detection
     * matches the connection to a specific strategy. */
    zend_fcall_t *handler =
        http_protocol_get_handler(&server->protocol_handlers, HTTP_PROTOCOL_HTTP1);

    if (handler == NULL) {
        handler = http_protocol_get_handler(&server->protocol_handlers, HTTP_PROTOCOL_HTTP2);
    }

    /* gRPC-only servers register no h1/h2 handler — accept anyway */
    const bool accept_grpc =
        http_protocol_has_handler(&server->protocol_handlers, HTTP_PROTOCOL_GRPC);

    if (UNEXPECTED(handler == NULL && server->static_handler_count == 0
                   && !accept_grpc)) {
        closesocket(client_fd);
        return;
    }

    /* Match the firing listen_event back to its row so we know whether
     * this accept belongs to a TLS listener AND which protocol mask it
     * carries. MAX_LISTENERS is tiny (16), the scan is effectively free. */
    tls_context_t *conn_tls_ctx = NULL;
    uint32_t conn_protocol_mask = server->view.protocol_mask;
    for (size_t i = 0; i < server->listener_count; i++) {
        if (server->listeners[i].listen_event != NULL
            && &server->listeners[i].listen_event->base == event) {
            conn_protocol_mask = server->listeners[i].protocol_mask;
#ifdef HAVE_OPENSSL
            if (server->listeners[i].tls) {
                conn_tls_ctx = server->tls_ctx;
            }
#endif
            break;
        }
    }

    if (!http_connection_spawn(
            client_fd,
            server->server_scope,
            handler,
            seconds_to_ms_clamped(server->read_timeout_s),
            seconds_to_ms_clamped(server->view.write_timeout_s),
            seconds_to_ms_clamped(server->keepalive_timeout_s),
            server,
            conn_tls_ctx,
            conn_protocol_mask)) {
        /* spawn() closed the fd itself; nothing to undo since we bump
         * active_connections only after success. */
        return;
    }

    server->active_connections++;
    /* total_requests is NOT bumped here — accept counts connections, not
     * requests. It lives in http_server_on_request_sample, fired once per
     * completed request (including each keep-alive request). */

    /* Hard-cap trigger: at or above pause_high, stop polling the listen
     * fd so new SYNs pile up in the kernel backlog rather than in us.
     * Overload → drain existing connections too. */
    if (!server->listeners_paused
        && server->pause_high > 0
        && server->active_connections >= server->pause_high) {
        http_server_pause_listeners(server, /*drain_connections=*/true);
    }
}

/* {{{ Built-in worker pool — issue #11.
 *
 * Each worker runs a C handler on a thread spawned via the C-level
 * pool->submit_internal API. One persistent shell per worker (the
 * snapshot deep-copy machinery is not concurrency-safe, so workers
 * cannot share). All shells live in a single pemalloc'd array on the
 * parent server; lifetime equals the parent's — http_server_free
 * releases the array. */

typedef struct {
    zval                server_transit;   /* per-worker persistent shell */
    zend_async_event_t *done_event;       /* completion future from submit_internal — owned here */
} pool_worker_ctx_t;

static void http_server_release_worker_shell(zval *transit);

typedef struct {
    int                          pending;     /* workers not yet done */
    zend_async_event_t          *all_done;    /* fires when pending == 0 */
    zend_async_event_callback_t  cb;          /* embedded — recovered via offsetof */
} pool_await_state_t;

static void pool_worker_handler(zend_async_event_t *event, void *vctx)
{
    (void)event;
    pool_worker_ctx_t *wctx = (pool_worker_ctx_t *)vctx;

    zval server_zv;
    ZVAL_UNDEF(&server_zv);
    ZEND_ASYNC_THREAD_LOAD_ZVAL_TOPLEVEL(&server_zv, &wctx->server_transit);

    if (EXPECTED(Z_TYPE(server_zv) == IS_OBJECT)) {
        zend_call_method_with_0_params(Z_OBJ(server_zv), NULL, NULL, "start", NULL);
        /* Exception in worker is captured by the future state via the
         * ext/async dispatcher — clear locally so the handler returns
         * cleanly. Log loudly first: a silently-cleared worker exception
         * means one whole thread's worth of accept capacity is gone for
         * the rest of the process lifetime, and we'd otherwise have no
         * visibility into the loss. */
        if (UNEXPECTED(EG(exception))) {
            zend_object  *ex      = EG(exception);
            const char   *cls     = ZSTR_VAL(ex->ce->name);
            zval         *msg_zv  = zend_read_property(ex->ce, ex,
                                        "message", sizeof("message") - 1,
                                        /*silent=*/1, NULL);
            const char   *msg     = (msg_zv != NULL && Z_TYPE_P(msg_zv) == IS_STRING)
                                        ? Z_STRVAL_P(msg_zv) : "";
            fprintf(stderr,
                "[true-async-server] worker thread died: %s: %s\n",
                cls, msg);
            fflush(stderr);
            zend_clear_exception();
        } else {
            /* normal on reload/stop; while running points at a swallowed bailout */
            fprintf(stderr,
                "[true-async-server] worker exited\n");
            fflush(stderr);
        }
    } else {
        fprintf(stderr,
            "[true-async-server] worker thread: server transfer failed\n");
        fflush(stderr);
    }

    zval_ptr_dtor(&server_zv);
    /* ctx is part of the parent's pool_worker_ctx array; freed once
     * by http_server_free when the parent server destructs. */
}

/* No-op: st->cb is embedded in pool_await_state and shared, so a disposed
 * future's callbacks_free() must not free it (a NULL dispose would also crash). */
static void pool_worker_cb_dispose(zend_async_event_callback_t *cb,
                                   zend_async_event_t *event)
{
    (void)cb; (void)event;
}

static void pool_worker_done_cb(zend_async_event_t *event,
                                zend_async_event_callback_t *cb,
                                void *result, zend_object *exception)
{
    (void)event; (void)result; (void)exception;
    /* Callbacks fire on the parent thread (cross-thread wakeup is
     * already serialized by the reactor) — no atomicity needed. */
    pool_await_state_t *st = (pool_await_state_t *)
        ((char *)cb - offsetof(pool_await_state_t, cb));

    if (--st->pending == 0 && st->all_done != NULL) {
        ZEND_ASYNC_CALLBACKS_NOTIFY(st->all_done, NULL, NULL);
    }
}

#ifndef PHP_WIN32
/* Close every pre-bound AF_UNIX socket and unlink its path. Run by the pool
 * parent after the pool has fully drained: each worker held its own dup, so
 * this is the final close that releases the socket. Idempotent. */
static void http_server_close_pool_unix_fds(http_server_object *server)
{
    for (size_t i = 0; i < server->pool_unix_fd_count; i++) {
        closesocket(server->pool_unix_fds[i].fd);
        unlink(server->pool_unix_fds[i].path);
    }
    server->pool_unix_fd_count = 0;
}

/* Bind every AF_UNIX listener once, before the worker pool is spawned, so all
 * workers can share a single listening socket (AF_UNIX has no SO_REUSEPORT —
 * N independent binds on one path is impossible). Each fd is recorded on the
 * server and copied to every worker by thread transfer; a worker adopts a
 * dup. Throws and returns FAILURE on the first bind error. */
static int http_server_prebind_unix(http_server_object *server)
{
    http_server_config_t *cfg = http_server_get_config(server);
    server->pool_unix_fd_count = 0;

    if (cfg == NULL) {
        return SUCCESS;
    }

    for (size_t i = 0; i < cfg->listener_count; i++) {
        http_listener_config_t *lc = &cfg->listeners[i];

        if (lc->type != LISTENER_TYPE_UNIX || lc->host == NULL) {
            continue;
        }

        if (server->pool_unix_fd_count >= MAX_LISTENERS) {
            break;
        }

        const char *path = ZSTR_VAL(lc->host);
        size_t      path_len = ZSTR_LEN(lc->host);

        struct sockaddr_un addr;
        if (path_len >= sizeof(addr.sun_path)) {
            http_server_close_pool_unix_fds(server);
            zend_throw_exception_ex(http_server_invalid_argument_exception_ce, 0,
                "Unix socket path too long: %s", path);
            return FAILURE;
        }

        http_server_unix_unlink_if_stale(path);

        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) {
            http_server_close_pool_unix_fds(server);
            zend_throw_exception_ex(http_server_runtime_exception_ce, 0,
                "Failed to create AF_UNIX socket for %s", path);
            return FAILURE;
        }

        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        memcpy(addr.sun_path, path, path_len + 1);

        if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0
            || listen(fd, cfg->backlog) != 0) {
            closesocket(fd);
            http_server_close_pool_unix_fds(server);
            zend_throw_exception_ex(http_server_runtime_exception_ce, 0,
                "Failed to bind AF_UNIX socket %s", path);
            return FAILURE;
        }

        http_pool_unix_fd_t *slot = &server->pool_unix_fds[server->pool_unix_fd_count++];
        memcpy(slot->path, path, path_len + 1);
        slot->fd = fd;
    }

    return SUCCESS;
}

/* Return the pre-bound listening fd for `path`, or -1 — i.e. this server is
 * not a pooled worker, or `path` is not one of the pre-bound unix sockets. */
static int http_server_pool_unix_fd_lookup(const http_server_object *server, const char *path)
{
    for (size_t i = 0; i < server->pool_unix_fd_count; i++) {
        if (strcmp(server->pool_unix_fds[i].path, path) == 0) {
            return server->pool_unix_fds[i].fd;
        }
    }
    return -1;
}
#endif  /* !PHP_WIN32 */

/* The TCP shared-fd path mirrors AF_UNIX above. Compiled on all POSIX
 * targets (not just non-REUSEPORT ones) so the path is reachable on Linux
 * when TRUE_ASYNC_SERVER_SHARED_LISTEN_FD forces it on for testing. */
#ifndef PHP_WIN32

static void http_server_close_pool_tcp_fds(http_server_object *server)
{
    for (size_t i = 0; i < server->pool_tcp_fd_count; i++) {
        closesocket(server->pool_tcp_fds[i].fd);
    }
    server->pool_tcp_fd_count = 0;
}

static int http_server_prebind_tcp(http_server_object *server)
{
    http_server_config_t *cfg = http_server_get_config(server);
    server->pool_tcp_fd_count = 0;

    if (cfg == NULL) {
        return SUCCESS;
    }

    for (size_t i = 0; i < cfg->listener_count; i++) {
        http_listener_config_t *lc = &cfg->listeners[i];

        if (lc->type != LISTENER_TYPE_TCP || lc->host == NULL) {
            continue;
        }

        if (server->pool_tcp_fd_count >= MAX_LISTENERS) {
            break;
        }

        const char *host = ZSTR_VAL(lc->host);
        char port_str[8];
        snprintf(port_str, sizeof(port_str), "%d", lc->port);

        struct addrinfo hints = {0};
        hints.ai_family   = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_flags    = AI_PASSIVE;

        struct addrinfo *res = NULL;
        if (getaddrinfo(host, port_str, &hints, &res) != 0 || res == NULL) {
            http_server_close_pool_tcp_fds(server);
            zend_throw_exception_ex(http_server_runtime_exception_ce, 0,
                "Failed to resolve %s:%d", host, lc->port);
            return FAILURE;
        }

        int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (fd < 0) {
            freeaddrinfo(res);
            http_server_close_pool_tcp_fds(server);
            zend_throw_exception_ex(http_server_runtime_exception_ce, 0,
                "Failed to create TCP socket for %s:%d", host, lc->port);
            return FAILURE;
        }

        int one = 1;
        (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

        if (bind(fd, res->ai_addr, res->ai_addrlen) != 0
            || listen(fd, cfg->backlog) != 0) {
            closesocket(fd);
            freeaddrinfo(res);
            http_server_close_pool_tcp_fds(server);
            zend_throw_exception_ex(http_server_runtime_exception_ce, 0,
                "Failed to bind TCP listener on %s:%d", host, lc->port);
            return FAILURE;
        }
        freeaddrinfo(res);

        http_pool_tcp_fd_t *slot = &server->pool_tcp_fds[server->pool_tcp_fd_count++];
        size_t host_len = strlen(host);
        if (host_len >= sizeof(slot->host)) host_len = sizeof(slot->host) - 1;
        memcpy(slot->host, host, host_len);
        slot->host[host_len] = '\0';
        slot->port = lc->port;
        slot->fd   = fd;
    }

    return SUCCESS;
}

static int http_server_pool_tcp_fd_lookup(const http_server_object *server,
                                          const char *host, int port)
{
    for (size_t i = 0; i < server->pool_tcp_fd_count; i++) {
        if (server->pool_tcp_fds[i].port == port
            && strcmp(server->pool_tcp_fds[i].host, host) == 0) {
            return server->pool_tcp_fds[i].fd;
        }
    }
    return -1;
}

#endif  /* !PHP_WIN32 */

/* Process-wide registry of worker inboxes. The pool parent creates it; worker
 * clones publish their inbox into it; reactor threads read it to pick a worker.
 * One per process, shared across all threads. */
static worker_registry_t *g_worker_registry = NULL;

/* Process-wide per-worker statistics slab (issue #5, A1). Created by the pool
 * parent sized to the worker count; each worker claims a slot (A2); the
 * telemetry API walks it lock-free. NULL outside pool mode — a single-worker
 * server reads its own embedded counters instead. Gating on setStatsEnabled
 * lands in A3; A1 always allocates it. */
static http_stats_registry_t *g_stats_registry = NULL;

/* Process-wide reactor pool handle. The pool itself is owned by the parent's
 * http_server_object; this global lets a worker thread reach it to post
 * responses back over the reverse channel (reactor_pool_post_exec), addressed by
 * the reactor_id carried on each request/response. One pool per process. */
static reactor_pool_t *g_reactor_pool = NULL;

/* Reactor pool opt-in gate. While the H3-listener-on-reactor wiring is
 * incomplete the pool is brought up only when TRUE_ASYNC_SERVER_REACTOR_POOL=1
 * so the default server behaves exactly as before. */
static bool http_server_reactor_pool_enabled(void)
{
    const char *env = getenv("TRUE_ASYNC_SERVER_REACTOR_POOL");
    return env != NULL && env[0] == '1';
}

/* Online CPU count, floor 1 — caps the reactor pool at the core count. */
static int http_server_online_cpus(void)
{
#ifdef PHP_WIN32
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return si.dwNumberOfProcessors > 0 ? (int)si.dwNumberOfProcessors : 1;
#else
    const long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 1;
#endif
}

/* True when the server config declares at least one HTTP/3 (QUIC) listener —
 * the reactor pool's only client today (the kernel ACKs TCP independently). */
static bool http_server_config_has_h3(http_server_object *server)
{
    http_server_config_t *const cfg = http_server_get_config(server);

    if (cfg == NULL) {
        return false;
    }

    for (size_t i = 0; i < cfg->listener_count; i++) {
        if (cfg->listeners[i].type == LISTENER_TYPE_UDP_H3) {
            return true;
        }
    }

    return false;
}

#ifdef HAVE_HTTP_SERVER_HTTP3
/* Exec payload: spawn one reactor-mode H3 listener ON the reactor thread — its
 * uv socket must be created on the loop that owns it (run via reactor_pool_exec).
 * server_obj is NULL (the reactor must not touch the parent's PHP object); the
 * thread-clean ctx drives config + worker routing. */
typedef struct {
    const char                *host;
    int                        port;
    void                      *ssl_ctx;
    const http3_reactor_ctx_t *ctx;
    http3_steer_group_t       *steer; /* endpoint's steering group, or NULL */
    http3_listener_t          *out;   /* result, NULL on failure */
} reactor_h3_spawn_arg_t;

static void reactor_h3_spawn_fn(void *arg)
{
    reactor_h3_spawn_arg_t *const spawn = (reactor_h3_spawn_arg_t *)arg;
    spawn->out = http3_listener_spawn(spawn->host, spawn->port, spawn->ssl_ctx, NULL, spawn->ctx);

    if (spawn->out == NULL && EG(exception)) {
        zend_clear_exception();   /* don't dangle on the reactor's EG */
    }

    /* Arm steering on the reactor's own thread before it processes traffic —
     * the listener already polls here, so this store happens-before any read in
     * try_steer on the same thread. */
    if (spawn->out != NULL && spawn->steer != NULL) {
        http3_listener_set_steer(spawn->out, spawn->steer);
    }
}

static void reactor_h3_destroy_fn(void *arg)
{
    http3_listener_destroy((http3_listener_t *)arg);
}

/* Spawn one reactor-mode H3 listener per (reactor x configured udp_h3 listener),
 * each on its reactor's own thread. Builds the parent-shared SSL_CTX + the
 * per-reactor thread-clean contexts (config scalars resolved here on the parent
 * where the server object is valid). Non-fatal: on failure the gated server just
 * doesn't serve H3. Returns the number spawned. */
static size_t http_server_reactor_h3_spawn(http_server_object *server, const int reactors)
{
    http_server_config_t *const cfg = http_server_get_config(server);

    if (cfg == NULL) {
        return 0;
    }

    /* QUIC mandates TLS — build the SSL_CTX on the parent (shared across reactor
     * threads; OpenSSL SSL_CTX is safe for concurrent per-connection SSL use). */
    char tls_err[TLS_ERR_BUF_SIZE];
    tls_err[0] = '\0';
    server->reactor_tls_ctx = tls_context_new(
        cfg->tls_cert_path ? ZSTR_VAL(cfg->tls_cert_path) : NULL,
        cfg->tls_key_path  ? ZSTR_VAL(cfg->tls_key_path)  : NULL,
        tls_err, sizeof(tls_err));

    if (server->reactor_tls_ctx == NULL) {
        fprintf(stderr, "[true-async-server] reactor H3 TLS context failed: %s\n",
                tls_err[0] != '\0' ? tls_err : "(no detail)");
        fflush(stderr);
        return 0;
    }

    server->reactor_h3_ctx =
        pecalloc((size_t)reactors, sizeof(http3_reactor_ctx_t), 1);

    const uint32_t socket_buffer_bytes = http_server_get_http3_socket_buffer_bytes(server);
    const uint32_t peer_budget         = http_server_get_http3_peer_connection_budget(server);
    const int      max_conns           = http_server_get_max_connections(server);
    const http_static_handler_t *const *const mounts = http_static_handler_mounts(server);
    const size_t   mount_count   = http_static_handler_count(server);

    /* Merge the open-file cache settings across mounts (max of max_entries,
     * min of non-zero ttl), same policy as http_static_cache_acquire. Each
     * reactor gets its own cache instance below. */
    int32_t cache_max = 0;
    int32_t cache_ttl = 0;
    for (size_t mi = 0; mi < mount_count; mi++) {
        const http_static_handler_t *const m = mounts[mi];

        if (m == NULL || m->cache_max_entries <= 0 || m->cache_ttl_seconds <= 0) {
            continue;
        }

        if (m->cache_max_entries > cache_max) {
            cache_max = m->cache_max_entries;
        }

        if (cache_ttl == 0 || m->cache_ttl_seconds < cache_ttl) {
            cache_ttl = m->cache_ttl_seconds;
        }
    }

    for (int r = 0; r < reactors; r++) {
        server->reactor_h3_ctx[r].registry            = g_worker_registry;
        server->reactor_h3_ctx[r].pool                = server->reactor_pool;
        server->reactor_h3_ctx[r].reactor_id          = r;
        server->reactor_h3_ctx[r].n_reactors          = reactors;
        server->reactor_h3_ctx[r].socket_buffer_bytes = socket_buffer_bytes;
        server->reactor_h3_ctx[r].peer_budget         = peer_budget;
        server->reactor_h3_ctx[r].max_conns           = max_conns > 0 ? (uint32_t)max_conns : 0;
        server->reactor_h3_ctx[r].static_mounts       = (const void *)mounts;
        server->reactor_h3_ctx[r].static_mount_count  = mount_count;
        server->reactor_h3_ctx[r].static_cache        =
            (cache_max > 0 && cache_ttl > 0)
                ? http_static_cache_create((size_t)cache_max, (time_t)cache_ttl)
                : NULL;
    }

    void *const ssl_ctx = server->reactor_tls_ctx->ctx;
    size_t spawned = 0;

    /* Steering engages only with >1 reactor (a single reactor owns every
     * connection — nothing to forward). Set process-wide before any listener
     * starts minting CIDs. */
    const bool steer_active = http3_steer_active();

    for (size_t i = 0; i < cfg->listener_count; i++) {
        if (cfg->listeners[i].type != LISTENER_TYPE_UDP_H3
            || cfg->listeners[i].host == NULL) {
            continue;
        }

        /* One steering group per endpoint, grouping its per-reactor listeners
         * by reactor id. Created up front so each listener can be armed with it
         * on its own reactor thread at spawn. */
        http3_steer_group_t *group =
            steer_active ? http3_steer_group_create(server->reactor_pool, reactors)
                         : NULL;
        size_t group_listeners = 0;

        for (int r = 0; r < reactors; r++) {
            if (server->reactor_h3_listener_count >= MAX_LISTENERS) {
                break;
            }

            reactor_h3_spawn_arg_t arg = {
                .host    = ZSTR_VAL(cfg->listeners[i].host),
                .port    = cfg->listeners[i].port,
                .ssl_ctx = ssl_ctx,
                .ctx     = &server->reactor_h3_ctx[r],
                .steer   = group,
                .out     = NULL,
            };

            if (!reactor_pool_exec(server->reactor_pool, r, reactor_h3_spawn_fn, &arg)
                || arg.out == NULL) {
                fprintf(stderr,
                    "[true-async-server] reactor H3 listener spawn failed "
                    "(reactor %d, %s:%d)\n", r, arg.host, arg.port);
                fflush(stderr);
                continue;
            }

            /* Publish the listener into its endpoint's steering table so sibling
             * reactors can forward to it (atomic — read lock-free on the
             * forward path). */
            http3_steer_group_publish(group, r, arg.out);
            group_listeners++;

            const size_t n = server->reactor_h3_listener_count++;
            server->reactor_h3_listeners[n].listener   = arg.out;
            server->reactor_h3_listeners[n].reactor_id = r;
            spawned++;
        }

        if (group != NULL && group_listeners > 0
            && server->reactor_h3_steer_count < MAX_LISTENERS) {
            server->reactor_h3_steer[server->reactor_h3_steer_count++] = group;
        } else {
            http3_steer_group_free(group);   /* no listeners, or no slot — drop it */
        }
    }

    return spawned;
}

/* Tear down every reactor-owned H3 listener on its own reactor thread (libuv
 * handles + ZMM allocated there), then free the contexts + shared SSL_CTX. Runs
 * before reactor_pool_destroy stops the reactors. */
static void http_server_reactor_h3_teardown(http_server_object *server)
{
    for (size_t i = 0; i < server->reactor_h3_listener_count; i++) {
        if (server->reactor_pool != NULL
            && server->reactor_h3_listeners[i].listener != NULL) {
            reactor_pool_exec(server->reactor_pool,
                              server->reactor_h3_listeners[i].reactor_id,
                              reactor_h3_destroy_fn,
                              server->reactor_h3_listeners[i].listener);
        }

        server->reactor_h3_listeners[i].listener = NULL;
    }

    server->reactor_h3_listener_count = 0;

    if (server->reactor_h3_ctx != NULL) {
        /* Destroy the per-reactor open-file caches. Listeners are already torn
         * down above (synchronously, on their reactors), so no reactor is still
         * serving; the caches are persistent (malloc), freed here on the parent. */
        const int rc = server->reactor_pool != NULL
            ? reactor_pool_count(server->reactor_pool) : 0;

        for (int r = 0; r < rc; r++) {
            if (server->reactor_h3_ctx[r].static_cache != NULL) {
                http_static_cache_destroy(server->reactor_h3_ctx[r].static_cache);
                server->reactor_h3_ctx[r].static_cache = NULL;
            }
        }

        pefree(server->reactor_h3_ctx, 1);
        server->reactor_h3_ctx = NULL;
    }

    if (server->reactor_tls_ctx != NULL) {
        tls_context_free(server->reactor_tls_ctx);
        server->reactor_tls_ctx = NULL;
    }
}
#endif /* HAVE_HTTP_SERVER_HTTP3 */

/* Bring up the transport reactor pool on the parent before workers run.
 * reactors = min(workers, cores) per the accepted R:W topology. No-op (and
 * leaves reactor_pool NULL) when the gate is off or no H3 listener is
 * configured. Non-fatal: a failed bring-up logs and the server runs without
 * it. */
static void http_server_reactor_pool_up(http_server_object *server, const int workers)
{
    server->reactor_pool = NULL;

    if (!http_server_reactor_pool_enabled() || !http_server_config_has_h3(server)) {
        return;
    }

    const int cores    = http_server_online_cpus();
    const int reactors = workers < cores ? workers : cores;

    const http_server_config_t *const cfg = http_server_get_config(server);
    const size_t mailbox_cap = cfg != NULL ? (size_t)cfg->reactor_mailbox_capacity : 0;

    server->reactor_pool = reactor_pool_create(reactors, mailbox_cap);

    if (server->reactor_pool == NULL) {
        /* reactor_pool_create may set a PHP error on hard failures; clear it
         * so the (non-fatal) gate does not poison the parent coroutine. */
        if (EG(exception)) {
            zend_clear_exception();
        }

        fprintf(stderr,
            "[true-async-server] reactor pool bring-up failed (reactors=%d) — "
            "continuing without it\n", reactors);
        fflush(stderr);
        return;
    }

    /* One inbox slot per worker — workers publish into it as they come up. */
    g_worker_registry = worker_registry_create(workers);
    g_reactor_pool    = server->reactor_pool;

    size_t h3_spawned = 0;
#ifdef HAVE_HTTP_SERVER_HTTP3
    /* Arm CID steering before any reactor mints a CID: encode the owner
     * reactor's id into every server CID so a migrated client rehashed by
     * SO_REUSEPORT onto another reactor routes back to its owner. Active only
     * with >1 reactor (the id is one byte, so cap at 256). */
    const int real_reactors = reactor_pool_count(server->reactor_pool);

    if (http3_steer_init()) {
        http3_steer_set_active(real_reactors > 1 && real_reactors <= 256);
        /* Flush forwarded datagrams once per reactor drain batch (not per
         * packet), so a burst of steered datagrams under rapid migration sends
         * like a recvmmsg tick instead of fragmenting a connection's output. */
        reactor_pool_set_drain_epilogue(http3_reactor_steer_flush_epilogue);
    }

    /* Spawn the H3 listeners ON the reactor threads now. From here the workers
     * stop spawning their own H3 listener (gated, in start()); the reactor owns
     * the transport and routes parsed requests to workers via the registry. */
    h3_spawned = http_server_reactor_h3_spawn(server, reactors);
#endif

    fprintf(stderr,
        "[true-async-server] reactor pool up: %d reactor(s), worker registry: %d slot(s), "
        "%zu H3 listener(s) on reactors\n",
        reactor_pool_count(server->reactor_pool),
        worker_registry_capacity(g_worker_registry),
        h3_spawned);
    fflush(stderr);
}

/* Tear the reactor pool down on the parent after workers have quiesced.
 * Idempotent; safe when the pool was never brought up. */
static void http_server_reactor_pool_down(http_server_object *server)
{
    /* Parent-only: a worker clone dying mid-run (hot reload rotation, #93)
     * must not tear down the GLOBAL registry/pool the live reactors and the
     * other workers are using. */
    if (server->is_worker_clone) {
        return;
    }

#ifdef HAVE_HTTP_SERVER_HTTP3
    /* Reactor-owned H3 listeners first, on their own threads, while the reactors
     * still run — then stop the pool. */
    http_server_reactor_h3_teardown(server);
#endif

    g_reactor_pool = NULL;

    if (g_worker_registry != NULL) {
        worker_registry_free(g_worker_registry);
        g_worker_registry = NULL;
    }

    if (server->reactor_pool != NULL) {
        reactor_pool_destroy(server->reactor_pool);
        server->reactor_pool = NULL;
    }

#ifdef HAVE_HTTP_SERVER_HTTP3
    /* Steering groups last: only safe once every reactor has stopped, since a
     * forward still queued on a reactor's mailbox reads the group's slots. */
    for (size_t i = 0; i < server->reactor_h3_steer_count; i++) {
        http3_steer_group_free(server->reactor_h3_steer[i]);
        server->reactor_h3_steer[i] = NULL;
    }

    server->reactor_h3_steer_count = 0;
#endif
}

#ifdef HAVE_HTTP_SERVER_HTTP3
/* Per-worker FIFO of reverse-path posts that didn't fit the reactor mailbox; a
 * 1 ms timer retries in order so the worker never blocks and a slot release
 * can't overtake a wire of its stream. discard runs on give-up (NULL = drop);
 * deadline_ns 0 = retry while the pool lives (releases), else a ~100 ms TTL. */
typedef struct pending_post_s {
    reactor_exec_fn        fn;
    void                  *arg;
    void                 (*discard)(void *arg);
    uint64_t               deadline_ns;
    int                    reactor;
    struct pending_post_s *next;
} pending_post_t;

ZEND_TLS pending_post_t *pending_post_head  = NULL;
ZEND_TLS pending_post_t *pending_post_tail  = NULL;
ZEND_TLS bool            pending_post_armed = false;

#define PENDING_WIRE_RETRY_MS    1u
#define PENDING_WIRE_TTL_NS      (100ull * 1000000ull)

static bool pending_post_arm_timer(void);

static void pending_discard_wire(void *arg)
{
    response_wire_discard((response_wire_t *)arg);
}

static void pending_post_flush(void)
{
    const uint64_t now = zend_hrtime();

    while (pending_post_head != NULL) {
        pending_post_t *const n = pending_post_head;

        const bool posted = g_reactor_pool != NULL
            && reactor_pool_post_exec(g_reactor_pool, n->reactor, n->fn, n->arg);

        if (!posted) {
            if (g_reactor_pool != NULL
                && (n->deadline_ns == 0 || now < n->deadline_ns)) {
                break;   /* mailbox still full — keep order, retry next tick */
            }
            if (n->discard != NULL) {
                n->discard(n->arg);   /* expired / pool gone → drop */
            }
        }

        pending_post_head = n->next;

        if (pending_post_head == NULL) {
            pending_post_tail = NULL;
        }

        pefree(n, 1);
    }

    if (pending_post_head != NULL && !pending_post_arm_timer()) {
        /* can't schedule another tick — fail the whole queue now */
        while (pending_post_head != NULL) {
            pending_post_t *const n = pending_post_head;
            if (n->discard != NULL) {
                n->discard(n->arg);
            }
            pending_post_head = n->next;
            pefree(n, 1);
        }
        pending_post_tail = NULL;
    }
}

static void pending_post_timer_fn(zend_async_event_t *event,
                                  zend_async_event_callback_t *callback,
                                  void *result, zend_object *exception)
{
    (void)event; (void)callback; (void)result;

    pending_post_armed = false;   /* one-shot: fired (auto-closes) */

    if (exception != NULL) {
        return;   /* loop teardown — flush would re-arm on a dying reactor */
    }

    pending_post_flush();
}

static bool pending_post_arm_timer(void)
{
    if (pending_post_armed) {
        return true;
    }

    zend_async_timer_event_t *const t =
        ZEND_ASYNC_NEW_TIMER_EVENT(PENDING_WIRE_RETRY_MS, /*periodic*/ false);

    if (UNEXPECTED(t == NULL)) {
        zend_clear_exception();
        return false;
    }

    zend_async_event_callback_t *const cb =
        ZEND_ASYNC_EVENT_CALLBACK(pending_post_timer_fn);

    if (UNEXPECTED(cb == NULL || !t->base.add_callback(&t->base, cb))) {
        if (cb != NULL) {
            ZEND_ASYNC_EVENT_CALLBACK_RELEASE(cb);
        }
        t->base.dispose(&t->base);
        return false;
    }

    if (UNEXPECTED(!t->base.start(&t->base))) {
        zend_async_callbacks_remove(&t->base, cb);
        t->base.dispose(&t->base);
        return false;
    }

    pending_post_armed = true;
    return true;
}

static bool pending_post_defer(reactor_exec_fn fn, void *arg,
                               void (*discard)(void *arg), int reactor,
                               uint64_t deadline_ns)
{
    pending_post_t *const n = pemalloc(sizeof(*n), 1);

    n->fn          = fn;
    n->arg         = arg;
    n->discard     = discard;
    n->reactor     = reactor;
    n->deadline_ns = deadline_ns;
    n->next        = NULL;

    if (pending_post_tail != NULL) {
        pending_post_tail->next = n;
    } else {
        pending_post_head = n;
    }
    pending_post_tail = n;

    if (!pending_post_arm_timer()) {
        /* unlink what we just queued; caller handles the drop */
        if (pending_post_head == n) {
            pending_post_head = NULL;
            pending_post_tail = NULL;
        } else {
            pending_post_t *p = pending_post_head;
            while (p->next != n) {
                p = p->next;
            }
            p->next = NULL;
            pending_post_tail = p;
        }
        pefree(n, 1);
        return false;
    }

    return true;
}
#endif /* HAVE_HTTP_SERVER_HTTP3 */

/* Worker response sink: post the rendered response back to the originating
 * reactor for nghttp3 encode + send. Runs on the worker thread (handler
 * coroutine or its dispose). reactor_id (echoed on the wire) selects the
 * reverse channel; ownership of `rw` transfers to the reactor apply on
 * success. A FULL wire that doesn't fit is dropped (the client times out);
 * STREAM_* wires are ordered fragments, so they defer to the hidden retry
 * timer instead — never blocking the worker thread. */
static bool http_server_worker_response_sink(response_wire_t *rw, void *arg)
{
    (void)arg;

#ifdef HAVE_HTTP_SERVER_HTTP3
    if (g_reactor_pool != NULL) {
        const int  reactor   = (int)response_wire_reactor_id(rw);
        const bool is_stream = response_wire_kind(rw) != RESPONSE_WIRE_FULL;

        /* Once anything is deferred, stream wires queue behind it so
         * fragments never overtake each other. FULL wires are whole
         * responses on their own streams — always try directly. */
        if ((!is_stream || pending_post_head == NULL)
            && reactor_pool_post_exec(g_reactor_pool, reactor,
                                      http3_reactor_apply_response, rw)) {
            return true;   /* the reactor owns rw now */
        }

        if (is_stream && pending_post_defer(http3_reactor_apply_response, rw,
                                            pending_discard_wire, reactor,
                                            zend_hrtime() + PENDING_WIRE_TTL_NS)) {
            return true;   /* parked; retried by the timer, ~100 ms TTL */
        }
    }
#endif

    /* undeliverable: abandoning the credit unblocks the producer */
    response_wire_discard(rw);
    return false;
}

/* Post a non-wire reactor op (H3 slot release) via the same ordered FIFO, no
 * TTL. False only if no retry timer can be armed, so the caller can fall back. */
bool http_worker_reactor_post_release(int reactor, void (*fn)(void *arg), void *arg)
{
#ifdef HAVE_HTTP_SERVER_HTTP3
    if (g_reactor_pool == NULL) {
        return false;
    }

    if (pending_post_head == NULL
        && reactor_pool_post_exec(g_reactor_pool, reactor, fn, arg)) {
        return true;
    }

    return pending_post_defer(fn, arg, /*discard*/ NULL, reactor, /*deadline*/ 0);
#else
    (void)reactor; (void)fn; (void)arg;
    return false;
#endif
}

/* Suspend the current coroutine for `ms` on a one-shot timer; lets the worker
 * loop keep draining (mailbox, scheduler) while we wait. */
static void hot_reload_sleep_ms(zend_coroutine_t *co, const zend_ulong ms)
{
    async_coroutine_sleep_ms(co, ms);

    if (EG(exception)) {
        zend_clear_exception();
    }
}

#ifdef HAVE_HTTP_SERVER_HTTP3
/* Flush deferred reverse posts before the worker loop dies — a parked slot
 * release must land or the reactor's slab keeps a live slot. Bounded. */
static void pending_post_drain(zend_coroutine_t *co)
{
    for (int spin = 0; spin < 400 && pending_post_head != NULL; spin++) {
        pending_post_flush();

        if (pending_post_head != NULL && co != NULL) {
            hot_reload_sleep_ms(co, 5);
        }
    }
}
#else
static void pending_post_drain(zend_coroutine_t *co) { (void)co; }
#endif

/* Reactor fence bookkeeping (#93): the decrement that reaches zero fires the
 * retiring worker's trigger — from a reactor thread, or from the worker itself
 * when a post could not be delivered. */
typedef struct {
    zend_atomic_int              remaining;
    zend_async_trigger_event_t  *done;
} inbox_fence_t;

static void http_server_inbox_fence_cb(void *arg)
{
    inbox_fence_t *const fence = arg;

    if (zend_atomic_int_fetch_sub(&fence->remaining, 1) == 1) {
        fence->done->trigger(fence->done);
    }
}

/* Unpublish this worker's inbox before the worker dies (#93 reload):
 * 1. retire the registry slot — no NEW pick can return the inbox;
 * 2. fence every reactor — a dispatch that loaded the pointer just before the
 *    retire has finished by the time its reactor runs the fence (dispatch is
 *    synchronous on the reactor loop, the fence queues behind it);
 * 3. wait out the residual mailbox — items already posted drain on our own
 *    loop into server_scope, where the normal stop-path drain awaits them.
 * After this the clone's worker_inbox_free really does run with quiesced
 * producers, which is the contract it always assumed. */
static void http_server_worker_inbox_retire(http_server_object *server)
{
    if (server->worker_inbox == NULL || g_worker_registry == NULL) {
        return;
    }

    worker_registry_retire(g_worker_registry, server->worker_inbox);

    zend_coroutine_t *const co = ZEND_ASYNC_CURRENT_COROUTINE;
    const int n = g_reactor_pool != NULL ? reactor_pool_count(g_reactor_pool) : 0;

    if (n > 0 && co != NULL) {
        zend_async_trigger_event_t *done = ZEND_ASYNC_NEW_TRIGGER_EVENT();

        if (done != NULL) {
            inbox_fence_t *fence = pemalloc(sizeof(*fence), 1);
            fence->done = done;
            ZEND_ATOMIC_INT_INIT(&fence->remaining, n);

            /* Subscribe BEFORE posting: a fence that fires instantly resolves
             * the waker and the suspend below returns immediately. */
            zend_async_resume_when(co, &done->base, false,
                                   zend_async_waker_callback_resolve, NULL);

            for (int i = 0; i < n; i++) {
                if (!reactor_pool_post_exec(g_reactor_pool, i,
                                            http_server_inbox_fence_cb, fence)) {
                    /* Undeliverable (reactor gone / mailbox full): take over its
                     * decrement; fire ourselves on the zero transition. */
                    if (zend_atomic_int_fetch_sub(&fence->remaining, 1) == 1) {
                        done->trigger(done);
                    }
                }
            }

            ZEND_ASYNC_SUSPEND();
            ZEND_ASYNC_WAKER_DESTROY(co);

            if (EG(exception)) {
                zend_clear_exception();
            }

            done->base.dispose(&done->base);
            pefree(fence, 1);
        }
    }

    /* Residual items already in the mailbox drain on our loop while we sleep;
     * bounded so a wedged reactor cannot park the rotation forever. */
    if (co != NULL) {
        for (int spin = 0; spin < 400 && worker_inbox_depth(server->worker_inbox) > 0; spin++) {
            hot_reload_sleep_ms(co, 5);
        }
    }

    /* land reverse posts those residual requests deferred (esp. slot releases) */
    pending_post_drain(co);

    http_logf_info(&server->log_state, "reload.inbox retired depth=%zu",
                   worker_inbox_depth(server->worker_inbox));
}

/* Claim this worker clone's stats slab slot and point its live counters at it,
 * so the telemetry API can sum every worker's counters. Idempotent — a reload
 * keeps the same slot — and a no-op for a standalone/parent server or when no
 * slab exists (manual ThreadPool mode). Runs on the worker thread at start,
 * before any connection caches the counters pointer. */
static void http_server_stats_up(http_server_object *server)
{
    if (server->stats_slot >= 0
        || g_stats_registry == NULL
        || !server->is_worker_clone) {
        return;
    }

    const int slot = http_stats_registry_claim(g_stats_registry);

    if (slot < 0) {
        return;
    }

    server->stats_slot    = slot;
    server->counters_live = &http_stats_registry_at(g_stats_registry, slot)->counters;
}

/* Release this worker's slab slot on teardown and drop its live counters back
 * to the embedded slice. Safe after the pool parent already freed the slab —
 * retire NULL-guards on the (now-cleared) registry. */
static void http_server_stats_down(http_server_object *server)
{
    if (server->stats_slot < 0) {
        return;
    }

    http_stats_registry_retire(g_stats_registry, server->stats_slot);
    server->stats_slot    = -1;
    server->counters_live = &server->counters;
}

#ifdef HTTP_SERVER_TEST_HOOKS
/* Test-only: snapshot each active slab slot's total_requests into out[] (up to
 * max), returning the active-slot count. Proves pool workers bump their own
 * slot rather than an embedded counter. Any thread (lock-free read). */
int http_server_stats_slab_snapshot(uint64_t *out, const int max)
{
    if (g_stats_registry == NULL) {
        return 0;
    }

    int n = 0;
    const int cap = http_stats_registry_capacity(g_stats_registry);

    for (int i = 0; i < cap; i++) {
        const http_stats_slot_t *const slot = http_stats_registry_at(g_stats_registry, i);

        if (!http_stats_slot_active(slot)) {
            continue;
        }

        if (out != NULL && n < max) {
            out[n] = slot->counters.total_requests;
        }

        n++;
    }

    return n;
}
#endif

/* Stand up this worker clone's request inbox and publish it to the shared
 * registry so a reactor can route requests to it. Gated + H3-only + clone-only;
 * a no-op otherwise. Runs on the worker thread after its server scope exists. */
static void http_server_worker_inbox_up(http_server_object *server)
{
    if (server->worker_inbox != NULL
        || g_worker_registry == NULL
        || !server->is_worker_clone
        || !http_server_reactor_pool_enabled()
        || !http_server_config_has_h3(server)
        || server->server_scope == NULL) {
        return;
    }

    server->worker_inbox = worker_inbox_create(server, server->server_scope,
                                               /*own_scope=*/true,
                                               http_server_worker_response_sink, NULL);

    if (server->worker_inbox == NULL) {
        return;
    }

    const int slot = worker_registry_add(g_worker_registry, server->worker_inbox);

    if (slot < 0) {
        worker_inbox_free(server->worker_inbox);
        server->worker_inbox = NULL;
        return;
    }

    fprintf(stderr,
        "[true-async-server] worker inbox published: slot %d of %d\n",
        slot, worker_registry_capacity(g_worker_registry));
    fflush(stderr);
}

static int http_server_start_pool(http_server_object *server,
                                  zval *this_zv,
                                  const int workers)
{
    if (UNEXPECTED(zend_async_new_thread_pool_fn == NULL)) {
        zend_throw_exception(http_server_runtime_exception_ce,
            "ThreadPool API is not registered — load true_async first", 0);
        return FAILURE;
    }

    /* Pull bootloader off the config (if any) and assemble a zend_fcall_t
     * for the pool factory. The factory deep-copies once into the
     * pool's snapshot, so the on-stack fcall has no lifetime beyond the
     * call itself. */
    http_server_config_t *boot_cfg = http_server_get_config(server);
    zend_fcall_t boot, *boot_ptr = NULL;
    if (boot_cfg != NULL
        && Z_TYPE(boot_cfg->bootloader) != IS_UNDEF
        && zend_fcall_info_init(&boot_cfg->bootloader, 0,
                                &boot.fci, &boot.fci_cache,
                                NULL, NULL) == SUCCESS) {
        boot_ptr = &boot;
    }

    /* The pool parent gets its own log lifecycle: reload() reports through it
     * (issue #93), and pool-mode start/stop become visible like worker ones. */
    if (boot_cfg != NULL) {
        http_server_start_logging(server, boot_cfg);
        http_logf_info(&server->log_state, "server.start mode=pool workers=%d",
                       workers);
    }

    /* queue_size = workers so submit doesn't block before workers reach
     * the receive loop — fresh-pool boot-up is otherwise faster on the
     * parent than on worker threads. */
    zend_async_thread_pool_t *pool =
        ZEND_ASYNC_NEW_THREAD_POOL_EX((int32_t)workers, (int32_t)workers,
                                      boot_ptr, /*coroutine_mode=*/false,
                                      /*concurrency=*/0);

    if (UNEXPECTED(pool == NULL || pool->submit_internal == NULL)) {
        if (pool != NULL) {
            ZEND_THREAD_POOL_DELREF(pool);
        }

        zend_throw_exception(http_server_runtime_exception_ce,
            "ThreadPool->submit_internal not available — true_async too old", 0);
        return FAILURE;
    }

#ifndef PHP_WIN32
    if (http_server_prebind_unix(server) != SUCCESS) {
        ZEND_THREAD_POOL_DELREF(pool);
        return FAILURE;
    }
#endif

#ifndef PHP_WIN32
    /* Shared-fd path: bind each TCP listener once, workers dup it. */
    if (http_server_use_shared_listen_fd()
        && http_server_prebind_tcp(server) != SUCCESS) {
        http_server_close_pool_unix_fds(server);
        ZEND_THREAD_POOL_DELREF(pool);
        return FAILURE;
    }
#endif

    /* Hot-reload beacon (issue #93): allocated BEFORE the transfer loop so
     * every worker shell fans the pointer out to its clone. */
    http_server_reload_shared_t *reload_shared = pecalloc(1, sizeof(*reload_shared), 1);
    ZEND_ATOMIC_INT_INIT(&reload_shared->epoch, 0);
    server->reload_shared = reload_shared;

    /* One persistent shell per worker. Allocate the whole array up
     * front so cleanup is a single pefree in http_server_free. */
    pool_worker_ctx_t *ctxs = pemalloc(sizeof(*ctxs) * (size_t)workers, 1);
    for (int i = 0; i < workers; i++) {
        ZVAL_UNDEF(&ctxs[i].server_transit);
        ctxs[i].done_event = NULL;
        ZEND_ASYNC_THREAD_TRANSFER_ZVAL_TOPLEVEL(&ctxs[i].server_transit, this_zv);

        if (UNEXPECTED(EG(exception))) {
            /* Roll back transfers we already did. */
            for (int j = 0; j <= i; j++) {
                http_server_release_worker_shell(&ctxs[j].server_transit);
            }

            pefree(ctxs, 1);
            server->reload_shared = NULL;
            pefree(reload_shared, 1);
            ZEND_THREAD_POOL_DELREF(pool);
#ifndef PHP_WIN32
            http_server_close_pool_unix_fds(server);
#endif
#ifndef PHP_WIN32
            http_server_close_pool_tcp_fds(server);
#endif
            return FAILURE;
        }
    }

    server->pool_worker_ctx       = ctxs;
    server->pool_worker_ctx_count = workers;

    /* Stand up the transport reactor pool + worker registry BEFORE submitting
     * workers, so a worker that comes up fast finds the registry ready to
     * publish into. */
    http_server_reactor_pool_up(server, workers);

    /* Per-worker stats slab (issue #5): one slot per worker, ready before any
     * worker claims one. Opt-in via setStatsEnabled; not gated behind the
     * reactor pool — telemetry applies to plain pool mode too. */
    const http_server_config_t *const stats_cfg = http_server_get_config(server);
    if (stats_cfg != NULL && stats_cfg->stats_enabled) {
        g_stats_registry = http_stats_registry_create(workers);
    }

    pool_await_state_t *st = ecalloc(1, sizeof(*st));
    st->pending = workers;
    st->all_done = create_server_wait_event();
    st->cb.callback = pool_worker_done_cb;
    st->cb.dispose  = pool_worker_cb_dispose;

    /* Retained for HttpServer::reload() (issue #93): it rotates worker_pool and
     * balances pool_await_state->pending for the fresh cohort. Cleared below
     * before the pool ref is dropped. */
    server->worker_pool      = pool;
    server->pool_await_state = st;

    int rc = FAILURE;
    for (int i = 0; i < workers; i++) {
        zend_async_event_t *worker_evt =
            pool->submit_internal(pool, pool_worker_handler, &ctxs[i]);

        if (UNEXPECTED(worker_evt == NULL)) {
            /* submit_internal failed mid-loop. Already-submitted workers
             * still hold &st->cb — we cannot free st right away without
             * a UAF. Mark remaining as already-done so the running
             * workers' callbacks fire NOTIFY on a still-valid st, then
             * fall through to the normal suspend/await path. */
            st->pending -= (workers - i);
            break;
        }

        zend_async_callbacks_push(worker_evt, &st->cb);
        ctxs[i].done_event = worker_evt;
    }

    server->in_pool_mode = true;
    server->running = true;

    /* Arm hot-reload triggers (issue #93) on the parent before it suspends. */
    if (boot_cfg != NULL
        && (Z_TYPE(boot_cfg->hot_reload_paths) == IS_ARRAY || boot_cfg->reload_on_sighup)) {
        http_server_hot_reload_up(server, this_zv, boot_cfg);
    }

    /* Suspend until all submitted workers report done. Skip the suspend
     * entirely when nothing got submitted — there is no callback to
     * notify all_done, so awaiting on it would deadlock. */
    if (st->pending > 0) {
        zend_coroutine_t *coroutine = ZEND_ASYNC_CURRENT_COROUTINE;

        if (UNEXPECTED(ZEND_ASYNC_WAKER_NEW(coroutine) == NULL)) {
            zend_throw_exception(http_server_runtime_exception_ce,
                "Failed to create waker for pool parent", 0);
            goto cleanup;
        }

        zend_async_resume_when(coroutine, st->all_done, true,
                               zend_async_waker_callback_resolve, NULL);
        ZEND_ASYNC_SUSPEND();
        /* resume_when(..., true) handed all_done to the waker; waker_clean
         * disposes it. NULL our pointer so the cleanup dispose below is a no-op
         * — disposing again is a use-after-free (same as server->wait_event). */
        st->all_done = NULL;
        zend_async_waker_clean(coroutine);

        if (EG(exception)) {
            zend_clear_exception();
        }
    }

    rc = (st->pending == 0) ? SUCCESS : FAILURE;

cleanup:
    /* Dispose the completion futures we own, else their cross-thread triggers
     * linger armed on our reactor. Live cohort, not `ctxs` — reload() swaps it. */
    {
        pool_worker_ctx_t *cur = (pool_worker_ctx_t *) server->pool_worker_ctx;
        for (int i = 0; cur != NULL && i < server->pool_worker_ctx_count; i++) {
            zend_async_event_t *ev = cur[i].done_event;

            if (ev == NULL) {
                continue;
            }

            cur[i].done_event = NULL;
            ZEND_ASYNC_EVENT_RELEASE(ev);
        }
    }

    server->running = false;
    server->stopping = false;
    server->in_pool_mode = false;
    server->worker_pool = NULL;
    server->pool_await_state = NULL;

    /* Retire the hot-reload orchestrators before the pool machinery goes. */
    http_server_hot_reload_down(server);

    /* Workers have quiesced (the suspend returned). Tear down the transport
     * reactor pool: this releases the H3 listeners the gated reactors own,
     * frees the worker registry, and stops the reactor loops. */
    http_server_reactor_pool_down(server);

    /* Workers have quiesced — free the stats slab after them (no claim/retire
     * can race the free now). */
    if (g_stats_registry != NULL) {
        http_stats_registry_free(g_stats_registry);
        g_stats_registry = NULL;
    }

    if (st->all_done != NULL) {
        st->all_done->dispose(st->all_done);
    }

    efree(st);
    ZEND_THREAD_POOL_DELREF(pool);

    /* Every clone (and its epoch-watching tick) is gone — drop the beacon. */
    server->reload_shared = NULL;
    pefree(reload_shared, 1);

    http_logf_info(&server->log_state, "server.stop mode=pool");
    http_log_server_stop(&server->log_state);

#ifndef PHP_WIN32
    /* Every worker has drained and closed its own dup; this final close
     * releases the shared sockets and removes their paths. */
    http_server_close_pool_unix_fds(server);
#endif
#ifndef PHP_WIN32
    http_server_close_pool_tcp_fds(server);
#endif
    return rc;
}
/* }}} */

/* {{{ proto HttpServer::start(): bool */
ZEND_METHOD(TrueAsync_HttpServer, start)
{
    ZEND_PARSE_PARAMETERS_NONE();

    http_server_object *server = Z_HTTP_SERVER_P(ZEND_THIS);

    if (server->running) {
        zend_throw_exception(http_server_runtime_exception_ce,
            "Server is already running", 0);
        RETURN_FALSE;
    }

    if (!http_protocol_has_handler(&server->protocol_handlers, HTTP_PROTOCOL_HTTP1) &&
        !http_protocol_has_handler(&server->protocol_handlers, HTTP_PROTOCOL_HTTP2) &&
        !http_protocol_has_handler(&server->protocol_handlers, HTTP_PROTOCOL_GRPC) &&
        server->static_handler_count == 0) {
        zend_throw_exception(http_server_runtime_exception_ce,
            "No HTTP handler registered. Call addHttpHandler(), "
            "addHttp2Handler(), addGrpcHandler(), or addStaticHandler() first", 0);
        RETURN_FALSE;
    }

    /* Check if TrueAsync scheduler is available */
    if (!zend_scheduler_is_enabled()) {
        zend_throw_exception(http_server_runtime_exception_ce,
            "TrueAsync scheduler is not available. Make sure async extension is loaded", 0);
        RETURN_FALSE;
    }

    /* Launch scheduler if not already active */
    if (!ZEND_ASYNC_IS_ACTIVE) {
        if (!ZEND_ASYNC_SCHEDULER_LAUNCH()) {
            zend_throw_exception(http_server_runtime_exception_ce,
                "Failed to launch TrueAsync scheduler", 0);
            RETURN_FALSE;
        }
    }

    /* Built-in worker pool dispatch (issue #11). When workers > 1 AND
     * we are not ourselves a worker clone constructed by transfer_obj,
     * delegate to http_server_start_pool which spawns Async\ThreadPool
     * and awaits every worker's standalone start(). Worker clones fall
     * through to the standalone path below. The scheduler must already
     * be active at this point — ThreadPool::__construct registers
     * worker threads with the reactor, and Future::await suspends the
     * calling coroutine. */
    if (!server->is_worker_clone) {
        zval workers_zv;
        ZVAL_UNDEF(&workers_zv);
        zend_call_method_with_0_params(Z_OBJ(server->config), NULL, NULL,
                                       "getWorkers", &workers_zv);
        const int workers_n = (Z_TYPE(workers_zv) == IS_LONG) ? (int)Z_LVAL(workers_zv) : 1;
        zval_ptr_dtor(&workers_zv);

        if (workers_n > 1) {
            if (http_server_start_pool(server, ZEND_THIS, workers_n) == SUCCESS) {
                RETURN_TRUE;
            }

            RETURN_FALSE;
        }
    }

    /* A pool worker claims its stats slab slot before any counter is touched,
     * so every subsequent read/write (and every conn that caches the pointer)
     * lands in the worker's own slot. */
    http_server_stats_up(server);

    /* Get config values - call PHP methods */
    zval retval;

    /* getBacklog */
    zend_call_method_with_0_params(Z_OBJ(server->config), NULL, NULL, "getBacklog", &retval);
    server->backlog = (int)Z_LVAL(retval);

    /* getMaxConnections */
    zend_call_method_with_0_params(Z_OBJ(server->config), NULL, NULL, "getMaxConnections", &retval);
    server->max_connections = (int)Z_LVAL(retval);

    /* getMaxInflightRequests — admission-reject cap. 0 from config =
     * "derive from max_connections": ×10 is a reasonable default (one
     * accepted conn on average keeps 10 requests alive on H2). When
     * max_connections is also 0 (unlimited), admission stays disabled
     * and the server falls back on CoDel/hard-cap only. */
    zend_call_method_with_0_params(Z_OBJ(server->config), NULL, NULL,
                                   "getMaxInflightRequests", &retval);
    size_t cfg_inflight = (size_t)Z_LVAL(retval);

    if (cfg_inflight == 0 && server->max_connections > 0) {
        cfg_inflight = (size_t)server->max_connections * 10u;
    }

    server->max_inflight_requests = cfg_inflight;
    server->counters_live->active_requests = 0;

    /* Derive backpressure thresholds from max_connections. pause_high = cap,
     * pause_low = 80% of cap (floor 1 below high). When max_connections=0
     * (unlimited) CoDel still runs as the adaptive trigger; hard-cap path
     * is disabled by its own pause_high > 0 guard. */
    if (server->max_connections > 0) {
        server->pause_high = (size_t)server->max_connections;
        server->pause_low  = (server->pause_high * BACKPRESSURE_PAUSE_LOW_RATIO) / 100;

        if (server->pause_low >= server->pause_high) {
            server->pause_low = server->pause_high > 0 ? server->pause_high - 1 : 0;
        }
    } else {
        server->pause_high = 0;
        server->pause_low  = 0;
    }

    server->listeners_paused = false;
    server->codel_window_start_ns = 0;
    server->codel_min_sojourn_ns = 0;
    server->codel_first_above_target_ns = 0;

    /* CoDel target from config (default 0 = off; sojourn-based AQM misfires
     * on HTTP/2 mux). CODEL_TARGET_MS env var still honoured as an
     * ops-friendly override — no rebuild needed to tune a running
     * deployment. 0 (either source) disables CoDel; hard-cap hysteresis
     * keeps working. */
    zend_call_method_with_0_params(Z_OBJ(server->config), NULL, NULL,
                                   "getBackpressureTargetMs", &retval);
    uint64_t codel_target_ms = (uint64_t)Z_LVAL(retval);
    const char *codel_env = getenv("CODEL_TARGET_MS");

    if (codel_env && *codel_env) {
        const long parsed = strtol(codel_env, NULL, 10);

        if (parsed >= 0 && parsed <= 10000) {
            codel_target_ms = (uint64_t)parsed;
        }
    }

    server->codel_target_ns = codel_target_ms * 1000000ULL;

    /* Timeouts: config setters validate range, so zend_long fits into uint32_t here. */
    zend_call_method_with_0_params(Z_OBJ(server->config), NULL, NULL, "getReadTimeout", &retval);
    server->read_timeout_s = (uint32_t)Z_LVAL(retval);

    zend_call_method_with_0_params(Z_OBJ(server->config), NULL, NULL, "getWriteTimeout", &retval);
    server->view.write_timeout_s = (uint32_t)Z_LVAL(retval);

    zend_call_method_with_0_params(Z_OBJ(server->config), NULL, NULL, "getKeepAliveTimeout", &retval);
    server->keepalive_timeout_s = (uint32_t)Z_LVAL(retval);

    zend_call_method_with_0_params(Z_OBJ(server->config), NULL, NULL, "getShutdownTimeout", &retval);
    server->shutdown_timeout_s = (uint32_t)Z_LVAL(retval);

    /* Drain knobs from config, converted to ns. 0 remains 0
     * (disabled per knob semantics). */
    zend_call_method_with_0_params(Z_OBJ(server->config), NULL, NULL, "getMaxConnectionAgeMs", &retval);
    server->max_connection_age_ns       = (uint64_t)Z_LVAL(retval) * 1000000ULL;

    zend_call_method_with_0_params(Z_OBJ(server->config), NULL, NULL, "getMaxConnectionAgeGraceMs", &retval);
    server->max_connection_age_grace_ns = (uint64_t)Z_LVAL(retval) * 1000000ULL;

    zend_call_method_with_0_params(Z_OBJ(server->config), NULL, NULL, "getDrainSpreadMs", &retval);
    server->drain_spread_ns             = (uint64_t)Z_LVAL(retval) * 1000000ULL;

    zend_call_method_with_0_params(Z_OBJ(server->config), NULL, NULL, "getDrainCooldownMs", &retval);
    server->drain_cooldown_ns           = (uint64_t)Z_LVAL(retval) * 1000000ULL;

    zend_call_method_with_0_params(Z_OBJ(server->config), NULL, NULL, "getStreamWriteBufferBytes", &retval);
    server->view.stream_write_buffer_bytes   = (uint32_t)Z_LVAL(retval);

    /* H3 knobs (NEXT_STEPS.md §5). Cached now so listener-spawn / connection-
     * accept paths can read without bouncing back through the userland
     * config object. */
    zend_call_method_with_0_params(Z_OBJ(server->config), NULL, NULL, "getHttp3IdleTimeoutMs", &retval);
    server->view.http3_idle_timeout_ms        = (uint32_t)Z_LVAL(retval);

    zend_call_method_with_0_params(Z_OBJ(server->config), NULL, NULL, "getHttp3StreamWindowBytes", &retval);
    server->view.http3_stream_window_bytes    = (uint32_t)Z_LVAL(retval);

    zend_call_method_with_0_params(Z_OBJ(server->config), NULL, NULL, "getHttp3MaxConcurrentStreams", &retval);
    server->view.http3_max_concurrent_streams = (uint32_t)Z_LVAL(retval);

    zend_call_method_with_0_params(Z_OBJ(server->config), NULL, NULL, "getHttp3PeerConnectionBudget", &retval);
    server->view.http3_peer_connection_budget = (uint32_t)Z_LVAL(retval);

    zend_call_method_with_0_params(Z_OBJ(server->config), NULL, NULL, "getHttp3SocketBufferBytes", &retval);
    server->view.http3_socket_buffer_bytes = (uint32_t)Z_LVAL(retval);

    zend_call_method_with_0_params(Z_OBJ(server->config), NULL, NULL, "getTlsBufferBytes", &retval);
    server->view.tls_buffer_bytes = (uint32_t)Z_LVAL(retval);

    zend_call_method_with_0_params(Z_OBJ(server->config), NULL, NULL, "isHttp3AltSvcEnabled", &retval);
    server->view.http3_alt_svc_enabled = (Z_TYPE(retval) == IS_TRUE);

    zend_call_method_with_0_params(Z_OBJ(server->config), NULL, NULL, "isHttp3Pacing", &retval);
    server->view.http3_pacing = (Z_TYPE(retval) == IS_TRUE);

    zend_call_method_with_0_params(Z_OBJ(server->config), NULL, NULL, "isRequestScope", &retval);
    server->view.request_scope = (Z_TYPE(retval) == IS_TRUE);

    zend_call_method_with_0_params(Z_OBJ(server->config), NULL, NULL, "isTelemetryEnabled", &retval);
    server->view.telemetry_enabled = (Z_TYPE(retval) == IS_TRUE);

    zend_call_method_with_0_params(Z_OBJ(server->config), NULL, NULL, "isBodyStreamingEnabled", &retval);
    server->view.body_streaming_enabled = (Z_TYPE(retval) == IS_TRUE);

    /* Stamps drive CoDel sojourn samples, the telemetry aggregate
     * (sojourn_sum / service_sum / sojourn_max) and the access-log duration.
     * Drain falls back to a fresh hrtime when end_ns is 0, so it does not
     * require stamps. */
    server->view.sample_stamps_enabled =
        (server->codel_target_ns != 0) || server->view.telemetry_enabled
        || server->log_state.has_access;

    /* Mirror configured max_body_size into the global parser pool.
     * Both the H1 parser (via http_parser_create on checkout) and the
     * H2 DATA-frame guard read from this global. Existing pooled parsers
     * carry the older limit, but pool starts empty at server construction
     * so this is a clean cutover. */
    zend_call_method_with_0_params(Z_OBJ(server->config), NULL, NULL, "getMaxBodySize", &retval);

    if (Z_LVAL(retval) > 0) {
        HTTP_SERVER_G(parser_pool).max_body_size = (size_t)Z_LVAL(retval);
    }

    server->view.drain_epoch_current  = 0;
    server->drain_last_fired_ns  = 0;

    /* Get listeners */
    zval listeners_zval;
    zend_call_method_with_0_params(Z_OBJ(server->config), NULL, NULL, "getListeners", &listeners_zval);

    if (Z_TYPE(listeners_zval) != IS_ARRAY || zend_hash_num_elements(Z_ARRVAL(listeners_zval)) == 0) {
        zval_ptr_dtor(&listeners_zval);
        zend_throw_exception(http_server_runtime_exception_ce,
            "No listeners configured. Use HttpServerConfig::addListener()", 0);
        RETURN_FALSE;
    }

    /* Create server scope WITH a zend_object.
     *
     * Each completed handler coroutine calls async_scope_notify_coroutine_finished
     * -> scope->try_to_dispose, and try_to_dispose consults scope_can_be_disposed
     * to decide whether to invoke scope_dispose. For a plain NEW_SCOPE (no
     * scope_object) and no CANCELLED flag, the check
     *
     *     can_be_disposed && !(CANCELLED || scope_object == NULL)  →  return false
     *
     * collapses to !(false || true) == false — i.e. dispose is NOT blocked,
     * and the scope gets freed as soon as it goes empty. For us that means:
     * first request finishes → scope freed → server->server_scope dangles →
     * second request's dispatch_request wild-jumps through the freed struct's
     * before_coroutine_enqueue pointer.
     *
     * Creating the scope WITH a zend_object makes scope_object non-NULL, so
     * the check above fires (return false) and try_to_dispose bails without
     * calling scope_dispose. The scope then stays alive for the whole server
     * lifetime, regardless of how many handler coroutines complete.
     *
     * We own one ref on the scope_object; releasing it in http_server_free
     * triggers scope_destroy which cascades to proper scope cleanup
     * (cancels any leftover coroutines, then disposes the struct). */
    server->server_scope = ZEND_ASYNC_NEW_SCOPE_WITH_OBJECT(ZEND_ASYNC_CURRENT_SCOPE);

    if (!server->server_scope) {
        zval_ptr_dtor(&listeners_zval);
        zend_throw_exception(http_server_runtime_exception_ce,
            "Failed to create server scope", 0);
        RETURN_FALSE;
    }

    /* Hard-terminate handler coroutines at shutdown instead of zombifying them:
     * clear DISPOSE_SAFELY (inherited from the main scope), else cancel() leaves
     * zombies and the shutdown drain's awaitAfterCancellation waits forever
     * (issue #74). Child request scopes inherit this non-safe mode. */
    ZEND_ASYNC_SCOPE_CLR_DISPOSE_SAFELY(server->server_scope);

    /* Keep our own pointer to the scope's zend_object. scope_destroy (the
     * dtor_obj handler) runs during request shutdown's dtor phase BEFORE
     * our http_server_free (the free_obj handler) runs — and it nulls
     * scope->scope_object, so we can't reach the object via server_scope
     * at that point. Stashing our own ref keeps it reachable until we
     * explicitly release it. */
    server->scope_object = server->server_scope->scope_object;

    /* Worker-pool clone under the reactor-pool gate: publish a request inbox so
     * a reactor can route parsed requests to this worker. No-op otherwise. */
    http_server_worker_inbox_up(server);

    /* Build TLS context up-front if any listener declared tls=true.
     * Doing this *before* binding sockets keeps the failure path cheap:
     * a bad cert means no listen_event allocation at all, and the
     * exception we throw carries OpenSSL's own diagnostic string. */
#ifdef HAVE_OPENSSL
    {
        bool any_tls_listener = false;
        zval *tls_probe;
        ZEND_HASH_FOREACH_VAL(Z_ARRVAL(listeners_zval), tls_probe) {
            if (Z_TYPE_P(tls_probe) != IS_ARRAY) {
                continue;
            }

            zval *tls_flag =
                zend_hash_str_find(Z_ARRVAL_P(tls_probe), "tls", 3);

            if (tls_flag != NULL && zend_is_true(tls_flag)) {
                any_tls_listener = true;
                break;
            }
        } ZEND_HASH_FOREACH_END();

        if (any_tls_listener) {
            zval cert_zv, key_zv;
            zend_call_method_with_0_params(Z_OBJ(server->config), NULL, NULL,
                                           "getCertificate", &cert_zv);
            zend_call_method_with_0_params(Z_OBJ(server->config), NULL, NULL,
                                           "getPrivateKey", &key_zv);

            const char *cert_path =
                (Z_TYPE(cert_zv) == IS_STRING) ? Z_STRVAL(cert_zv) : NULL;
            const char *key_path =
                (Z_TYPE(key_zv)  == IS_STRING) ? Z_STRVAL(key_zv)  : NULL;

            char tls_err[TLS_ERR_BUF_SIZE];
            tls_err[0] = '\0';
            server->tls_ctx = tls_context_new(cert_path, key_path,
                                              tls_err, sizeof(tls_err));

            zval_ptr_dtor(&cert_zv);
            zval_ptr_dtor(&key_zv);

            if (server->tls_ctx == NULL) {
                zval_ptr_dtor(&listeners_zval);
                zend_throw_exception(http_server_runtime_exception_ce,
                    tls_err[0] != '\0'
                        ? tls_err
                        : "Failed to initialise TLS context",
                    0);
                RETURN_FALSE;
            }
        }
    }
#else
    /* Built without OpenSSL — refuse TLS at start() rather than
     * silently falling back to plaintext on a listener that asked for
     * encryption. Mirrors the pre-step-2 behaviour of enableTls(). */
    {
        zval *tls_probe;
        ZEND_HASH_FOREACH_VAL(Z_ARRVAL(listeners_zval), tls_probe) {
            if (Z_TYPE_P(tls_probe) != IS_ARRAY) {
                continue;
            }

            zval *tls_flag =
                zend_hash_str_find(Z_ARRVAL_P(tls_probe), "tls", 3);

            if (tls_flag != NULL && zend_is_true(tls_flag)) {
                zval_ptr_dtor(&listeners_zval);
                zend_throw_exception(http_server_runtime_exception_ce,
                    "TLS listener requested but extension was built "
                    "without OpenSSL support", 0);
                RETURN_FALSE;
            }
        } ZEND_HASH_FOREACH_END();
    }
#endif

    /* Create listen sockets for each listener */
    zval *listener;
    server->listener_count = 0;

    ZEND_HASH_FOREACH_VAL(Z_ARRVAL(listeners_zval), listener) {
        if (server->listener_count >= MAX_LISTENERS) {
            break;
        }

        if (Z_TYPE_P(listener) != IS_ARRAY) {
            continue;
        }

        zval *type_zv = zend_hash_str_find(Z_ARRVAL_P(listener), "type", 4);

        if (!type_zv || Z_TYPE_P(type_zv) != IS_STRING) {
            continue;
        }

        if (strcmp(Z_STRVAL_P(type_zv), "tcp") == 0) {
            zval *host_zv = zend_hash_str_find(Z_ARRVAL_P(listener), "host", 4);
            zval *port_zv = zend_hash_str_find(Z_ARRVAL_P(listener), "port", 4);
            zval *tls_zv = zend_hash_str_find(Z_ARRVAL_P(listener), "tls", 3);
            zval *mask_zv = zend_hash_str_find(Z_ARRVAL_P(listener), "protocol_mask", sizeof("protocol_mask") - 1);

            if (!host_zv || !port_zv) continue;

            const char *host = Z_STRVAL_P(host_zv);
            int port = (int)Z_LVAL_P(port_zv);
            bool tls = tls_zv && zend_is_true(tls_zv);
            uint32_t protocol_mask = (mask_zv && Z_TYPE_P(mask_zv) == IS_LONG)
                ? (uint32_t)Z_LVAL_P(mask_zv)
                : (HTTP_PROTO_MASK_HTTP1 | HTTP_PROTO_MASK_HTTP2);

            unsigned int listen_flags = 0;
            zend_async_listen_event_t *listen_event = NULL;

            /* Strategies for a worker pool sharing host:port. REUSEPORT:
             * each worker binds independently, the kernel load-balances
             * (Linux/FreeBSD). Shared fd: the parent bound once and each
             * worker adopts a dup (macOS/other BSD, no LB REUSEPORT).
             * Windows has neither — uv_tcp_bind() rejects UV_TCP_REUSEPORT
             * with ENOTSUP and there is no POSIX dup to share — so it takes
             * the plain-bind fall-through below (single listener only). */
            if (http_server_use_reuseport()) {
                listen_flags |= ZEND_ASYNC_LISTEN_F_REUSEPORT;
            }
#ifndef PHP_WIN32
            else {
                const int prebound_fd =
                    http_server_pool_tcp_fd_lookup(server, host, port);

                if (prebound_fd >= 0) {
                    const int dup_fd = dup(prebound_fd);

                    if (dup_fd < 0) {
                        zend_throw_exception_ex(http_server_runtime_exception_ce, 0,
                            "Failed to dup TCP listen fd for %s:%d", host, port);
                    } else {
                        listen_event = ZEND_ASYNC_SOCKET_LISTEN_FD(
                            dup_fd, server->backlog, listen_flags, 0);
                    }
                }
            }
#endif

            if (listen_event == NULL && !EG(exception)) {
                listen_event = ZEND_ASYNC_SOCKET_LISTEN_EX(
                    host, port, server->backlog, listen_flags, 0);
            }

            if (!listen_event) {
                /* Cleanup already-created listen events */
                for (size_t i = 0; i < server->listener_count; i++) {
                    http_server_listener_release(&server->listeners[i]);
                }

                server->listener_count = 0;
#ifdef HAVE_OPENSSL
                /* Drop the TLS context so a subsequent start() rebuilds
                 * it instead of leaking the partially-wired one. */
                if (server->tls_ctx != NULL) {
                    tls_context_free(server->tls_ctx);
                    server->tls_ctx = NULL;
                }
#endif
                zval_ptr_dtor(&listeners_zval);
                if (!EG(exception)) {
                    zend_throw_exception_ex(http_server_runtime_exception_ce, 0,
                        "Failed to bind listener on %s:%d", host, port);
                }
                RETURN_THROWS();
            }

            server->listeners[server->listener_count].listen_event = listen_event;
            server->listeners[server->listener_count].tls = tls;
            server->listeners[server->listener_count].protocol_mask = protocol_mask;
            server->listener_count++;

            listen_event->base.add_callback(&listen_event->base,
                ZEND_ASYNC_EVENT_CALLBACK(http_server_accept_callback));

            listen_event->base.start(&listen_event->base);
        }
#ifdef HAVE_HTTP_SERVER_HTTP3
        else if (strcmp(Z_STRVAL_P(type_zv), "udp_h3") == 0) {
            /* Under the reactor-pool gate the transport reactor owns the H3
             * listener (spawned by the parent in http_server_reactor_pool_up);
             * a worker clone must NOT spawn its own, or two listeners would
             * REUSEPORT-share the socket and the reactor split would not hold.
             * The worker still publishes its request inbox for the reactor to
             * route to. */
            if (http_server_reactor_pool_enabled() && server->is_worker_clone) {
                continue;
            }

            if (server->http3_listener_count >= MAX_LISTENERS) {
                continue;
            }

            zval *host_zv = zend_hash_str_find(Z_ARRVAL_P(listener), "host", 4);
            zval *port_zv = zend_hash_str_find(Z_ARRVAL_P(listener), "port", 4);

            if (!host_zv || !port_zv) continue;

            /* QUIC mandates TLS — we asserted any_tls_listener above and
             * server->tls_ctx was built. Hand the SSL_CTX* to the listener
             * so it can spin per-connection SSLs via ngtcp2_crypto_ossl. */
# ifdef HAVE_OPENSSL
            void *ssl_ctx = (server->tls_ctx != NULL) ? server->tls_ctx->ctx : NULL;
# else
            void *ssl_ctx = NULL;
# endif
            http3_listener_t *h3 = http3_listener_spawn(
                Z_STRVAL_P(host_zv), (int)Z_LVAL_P(port_zv), ssl_ctx,
                /* server_obj: */ server, /* reactor_ctx: */ NULL);
            if (!h3) {
                /* Unwind both TCP and H3 listeners — start() is all-or-nothing. */
                for (size_t i = 0; i < server->listener_count; i++) {
                    http_server_listener_release(&server->listeners[i]);
                }

                server->listener_count = 0;
                for (size_t i = 0; i < server->http3_listener_count; i++) {
                    http3_listener_destroy(server->http3_listeners[i]);
                    server->http3_listeners[i] = NULL;
                }

                server->http3_listener_count = 0;
# ifdef HAVE_OPENSSL
                if (server->tls_ctx != NULL) {
                    tls_context_free(server->tls_ctx);
                    server->tls_ctx = NULL;
                }
# endif
                zval_ptr_dtor(&listeners_zval);
                RETURN_FALSE;
            }

            server->http3_listeners[server->http3_listener_count++] = h3;
        }
#else
        else if (strcmp(Z_STRVAL_P(type_zv), "udp_h3") == 0) {
            zval_ptr_dtor(&listeners_zval);
            zend_throw_exception(http_server_runtime_exception_ce,
                "HTTP/3 listener requested but extension was built without "
                "--enable-http3 support", 0);
            RETURN_FALSE;
        }
#endif
        else if (strcmp(Z_STRVAL_P(type_zv), "unix") == 0) {
            if (server->listener_count >= MAX_LISTENERS) {
                continue;
            }

            zval *path_zv = zend_hash_str_find(Z_ARRVAL_P(listener), "path", 4);
            zval *mask_zv = zend_hash_str_find(Z_ARRVAL_P(listener), "protocol_mask", sizeof("protocol_mask") - 1);

            if (!path_zv || Z_TYPE_P(path_zv) != IS_STRING) continue;

            uint32_t protocol_mask = (mask_zv && Z_TYPE_P(mask_zv) == IS_LONG)
                ? (uint32_t)Z_LVAL_P(mask_zv)
                : (HTTP_PROTO_MASK_HTTP1 | HTTP_PROTO_MASK_HTTP2);

            zend_async_listen_event_t *listen_event = NULL;
            bool unlink_on_close = false;

#ifndef PHP_WIN32
            /* Pooled worker: the parent already bound this path. Adopt a
             * private dup of the shared fd — the parent owns the path and
             * the original fd, and unlinks once the pool drains. */
            const int prebound_fd =
                http_server_pool_unix_fd_lookup(server, Z_STRVAL_P(path_zv));

            if (prebound_fd >= 0) {
                const int dup_fd = dup(prebound_fd);

                if (dup_fd < 0) {
                    zend_throw_exception_ex(http_server_runtime_exception_ce, 0,
                        "Failed to dup AF_UNIX listen fd for %s", Z_STRVAL_P(path_zv));
                } else {
                    listen_event = ZEND_ASYNC_SOCKET_LISTEN_FD(
                        dup_fd, server->backlog, ZEND_ASYNC_LISTEN_F_UNIX, 0);
                }
            }
#endif

            if (listen_event == NULL && !EG(exception)) {
                /* Single-worker server: bind the path directly and own it.
                 * The reactor binds a uv_pipe_t; accepted connections flow
                 * through http_server_accept_callback exactly like TCP. */
#ifndef PHP_WIN32
                /* Drop a leftover socket file from a crashed prior run so
                 * bind() does not fail with EADDRINUSE on our own stale path. */
                http_server_unix_unlink_if_stale(Z_STRVAL_P(path_zv));
#endif
                listen_event = ZEND_ASYNC_SOCKET_LISTEN_EX(
                    Z_STRVAL_P(path_zv), 0, server->backlog, ZEND_ASYNC_LISTEN_F_UNIX, 0);
                unlink_on_close = true;
            }

            if (!listen_event) {
                /* Unwind every listener created so far — start() is
                 * all-or-nothing. */
                for (size_t i = 0; i < server->listener_count; i++) {
                    http_server_listener_release(&server->listeners[i]);
                }

                server->listener_count = 0;
#ifdef HAVE_HTTP_SERVER_HTTP3
                for (size_t i = 0; i < server->http3_listener_count; i++) {
                    http3_listener_destroy(server->http3_listeners[i]);
                    server->http3_listeners[i] = NULL;
                }

                server->http3_listener_count = 0;
#endif
#ifdef HAVE_OPENSSL
                if (server->tls_ctx != NULL) {
                    tls_context_free(server->tls_ctx);
                    server->tls_ctx = NULL;
                }
#endif
                zval_ptr_dtor(&listeners_zval);
                /* An exception is already pending — from the dup() failure
                 * above, or thrown by the reactor with the uv_strerror. */
                RETURN_FALSE;
            }

            server->listeners[server->listener_count].listen_event = listen_event;
            server->listeners[server->listener_count].tls = false;
            server->listeners[server->listener_count].unlink_on_close = unlink_on_close;
            server->listeners[server->listener_count].protocol_mask = protocol_mask;
            server->listener_count++;

            listen_event->base.add_callback(&listen_event->base,
                ZEND_ASYNC_EVENT_CALLBACK(http_server_accept_callback));

            listen_event->base.start(&listen_event->base);
        }
    } ZEND_HASH_FOREACH_END();

    zval_ptr_dtor(&listeners_zval);

    size_t total_listeners = server->listener_count;
#ifdef HAVE_HTTP_SERVER_HTTP3
    total_listeners += server->http3_listener_count;
#endif
    if (total_listeners == 0) {
        zend_throw_exception(http_server_runtime_exception_ce,
            "No valid listeners could be created", 0);
        RETURN_FALSE;
    }

#ifdef HAVE_HTTP_SERVER_HTTP3
    /* Pre-compute the Alt-Svc header value once, while we
     * have visibility into the H3 listener set. Format per RFC 7838:
     *   Alt-Svc: h3=":<port>"; ma=86400
     * Multiple H3 listeners → first one wins (clients almost never act
     * on the second alternative anyway; chaining is a future polish).
     * PHP_HTTP3_DISABLE_ALT_SVC=1 disables emission for operators who
     * are still rolling H3 out and don't want clients migrating yet. */
    if (server->alt_svc_header_value != NULL) {
        zend_string_release(server->alt_svc_header_value);
        server->alt_svc_header_value = NULL;
    }

    if (server->http3_listener_count > 0
        && http_server_get_http3_alt_svc_enabled(server)
        && getenv("PHP_HTTP3_DISABLE_ALT_SVC") == NULL) {
        const int port = http3_listener_port(server->http3_listeners[0]);
        char buf[64];
        const int n = snprintf(buf, sizeof(buf),
            "h3=\":%d\"; ma=86400", port);

        if (n > 0 && n < (int)sizeof(buf)) {
            server->alt_svc_header_value = zend_string_init(buf, (size_t)n, 0);
        }
    }
#endif

    {
        http_server_config_t *cfg = http_server_config_from_obj(Z_OBJ(server->config));
        http_server_start_logging(server, cfg);
        http_logf_info(&server->log_state,
                       "server.start backlog=%d max_connections=%d",
                       server->backlog, server->max_connections);
    }

    server->running = true;
    current_server = server;

    /* Per-worker deadline watchdog: one periodic timer that walks the
     * conn arena's alive list each tick and force-closes idle conns
     * whose deadline_ns has elapsed. Replaces per-conn write_timer +
     * per-await read-timeout creation. Failure here is non-fatal. */
    http_server_deadline_tick_start(server);

    /* Create wait event and suspend until stop() is called */
    zend_coroutine_t *coroutine = ZEND_ASYNC_CURRENT_COROUTINE;

    server->wait_event = create_server_wait_event();

    /* Create waker for coroutine */
    if (ZEND_ASYNC_WAKER_NEW(coroutine) == NULL) {
        server->wait_event->dispose(server->wait_event);
        server->wait_event = NULL;
        zend_throw_exception(http_server_runtime_exception_ce,
            "Failed to create waker for server", 0);
        RETURN_FALSE;
    }

    /* Attach coroutine to wait event - will suspend until stop() resolves it */
    zend_async_resume_when(coroutine, server->wait_event, true,
                           zend_async_waker_callback_resolve, NULL);

    /* Suspend coroutine - control returns to event loop. zend_try is
     * the only way to run deadline_tick_stop on a bailout exit too —
     * otherwise longjmp skips it and the periodic timer keeps libuv
     * loop alive past worker shutdown (scheduler.c:1964 asserts). */
    volatile bool bailout = false;
    zend_try {
        ZEND_ASYNC_SUSPEND();
        /* The waker owns the wait_event (resume_when took ownership) and
         * waker_clean disposes it below — drop our pointer first so a late
         * stop() on a coroutine torn down by cancellation (not by stop())
         * does not notify a freed event. */
        server->wait_event = NULL;
        zend_async_waker_clean(coroutine);

        if (EG(exception)) {
            zend_clear_exception();
        }
    } zend_catch {
        bailout = true;
    } zend_end_try();

    /* Pair to deadline_tick_start above — same lifecycle, runs on every
     * exit (normal, cancellation, bailout). Without this the periodic
     * timer keeps libuv loop alive past worker shutdown and
     * scheduler.c:1964 asserts ("The event loop must be stopped"). */
    http_server_deadline_tick_stop(server);

    if (UNEXPECTED(bailout)) {
        zend_bailout();
    }

    /* Drain in-flight per-request handler coroutines now, while we are still
     * on the start() coroutine and can suspend, so server_scope is empty when
     * the object is freed (issue #74). On bailout we already longjmp'd above. */
    http_server_drain_scope(server);

    RETURN_TRUE;
}
/* }}} */

/* stop() core, shared by the PHP method and the hot-reload self-stop coroutine
 * (issue #93). Never suspends. `reason` != NULL tags the log line. */
static void http_server_do_stop(http_server_object *server, const char *reason)
{
    /* Stop the deadline watchdog FIRST so the periodic timer no longer
     * keeps the libuv loop alive past server stop. Without this, the
     * loop drains forever and stop() never lets the script exit. */
    http_server_deadline_tick_stop(server);

    /* Stop all listen events. dispose() closes the underlying uv handle
     * and frees the fd; AF_UNIX rows also unlink the socket file. */
    for (size_t i = 0; i < server->listener_count; i++) {
        http_server_listener_release(&server->listeners[i]);
    }

#ifdef HAVE_HTTP_SERVER_HTTP3
    for (size_t i = 0; i < server->http3_listener_count; i++) {
        if (server->http3_listeners[i]) {
            http3_listener_destroy(server->http3_listeners[i]);
            server->http3_listeners[i] = NULL;
        }
    }

    server->http3_listener_count = 0;
#endif

    /* Logger teardown must precede the wait_event notify: waking
     * start() can let the scheduler tear down before our async write
     * completes, leaving the spawn coroutine here stuck mid-await. */
    if (reason != NULL) {
        http_logf_info(&server->log_state, "server.stop reason=%s", reason);
    } else {
        http_logf_info(&server->log_state, "server.stop");
    }

    http_log_server_stop(&server->log_state);

    /* Resolve wait event to wake up start(). The event is released by the
     * server coroutine's waker_clean on resume — do NOT dispose here, that
     * produces a use-after-free (see server_wait_event_dispose). */
    if (server->wait_event) {
        zend_async_event_t *wait = server->wait_event;
        server->wait_event = NULL;
        ZEND_ASYNC_CALLBACKS_NOTIFY(wait, NULL, NULL);
    }

    /* TODO: Wait for active connections with timeout
     * Then cancel server scope to terminate all connection coroutines */

    server->running = false;
    server->stopping = false;
    current_server = NULL;

    /* Release pooled body buffers held by this thread back to zend_mm.
     * Workers that don't accept further requests don't need their pool
     * any more; keeping it would just inflate RSS until module shutdown. */
    body_pool_shutdown();
#ifdef HAVE_HTTP_COMPRESSION
    http_compression_pool_shutdown();
#endif
}

/* {{{ proto HttpServer::stop(): bool */
ZEND_METHOD(TrueAsync_HttpServer, stop)
{
    ZEND_PARSE_PARAMETERS_NONE();

    http_server_object *server = Z_HTTP_SERVER_P(ZEND_THIS);

    if (!server->running) {
        RETURN_TRUE;
    }

    server->stopping = true;

    /* Pool-mode parent has no listen events of its own — it's awaiting
     * worker-completion callbacks. Cross-thread shutdown isn't wired up
     * yet (issue #11): each worker has to stop itself from within its
     * own handler. */
    if (server->in_pool_mode) {
        zend_throw_exception(http_server_runtime_exception_ce,
            "stop() on a pool-parent HttpServer is not supported yet "
            "(issue #11). Stop each worker from within its own handler.", 0);
        RETURN_FALSE;
    }

    http_server_do_stop(server, NULL);
    RETURN_TRUE;
}
/* }}} */

/* {{{ proto HttpServer::reload(): bool
 * Hot reload (issue #93), pool parent only. Bumps the shared epoch (workers
 * self-stop from their deadline tick: drain #74 + stop + exit to the closed
 * pool channel), rotates the pool via ThreadPool ABI reload() — replacement
 * threads re-run the bootloader, picking up changed code — then resubmits one
 * start() task per worker onto the fresh channel. Suspends until the old
 * cohort has fully drained. */
ZEND_METHOD(TrueAsync_HttpServer, reload)
{
    ZEND_PARSE_PARAMETERS_NONE();

    http_server_object *server = Z_HTTP_SERVER_P(ZEND_THIS);

    if (UNEXPECTED(!server->in_pool_mode || server->worker_pool == NULL
                   || server->pool_await_state == NULL)) {
        zend_throw_exception(http_server_runtime_exception_ce,
            "reload() requires the built-in worker pool "
            "(config->setWorkers(N) and a running start())", 0);
        RETURN_THROWS();
    }

    if (UNEXPECTED(server->worker_pool->reload == NULL)) {
        zend_throw_exception(http_server_runtime_exception_ce,
            "true_async is too old for hot reload (ThreadPool ABI < 0.22)", 0);
        RETURN_THROWS();
    }


    if (server->reload_in_progress) {
        RETURN_FALSE; /* one rotation at a time — call again after it returns */
    }

    server->reload_in_progress = true;

    const int workers = server->pool_worker_ctx_count;
    pool_await_state_t *st = server->pool_await_state;
    http_server_reload_shared_t *shared = server->reload_shared;
    const uint64_t t0 = ZEND_ASYNC_NOW();

    http_logf_info(&server->log_state, "reload.start workers=%d", workers);

    /* Transit shells are single-load (the clone consumes the persistent closure
     * copies), so every replacement cohort needs a fresh transfer. Done BEFORE
     * anything is disrupted — a transfer failure aborts the reload cleanly. */
    pool_worker_ctx_t *fresh = pemalloc(sizeof(*fresh) * (size_t)workers, 1);

    for (int i = 0; i < workers; i++) {
        ZVAL_UNDEF(&fresh[i].server_transit);
        fresh[i].done_event = NULL;
        ZEND_ASYNC_THREAD_TRANSFER_ZVAL_TOPLEVEL(&fresh[i].server_transit, ZEND_THIS);

        if (UNEXPECTED(EG(exception))) {
            for (int j = 0; j <= i; j++) {
                http_server_release_worker_shell(&fresh[j].server_transit);
            }

            pefree(fresh, 1);
            http_logf_info(&server->log_state, "reload.failed stage=transfer");
            server->reload_in_progress = false;
            RETURN_THROWS();
        }
    }

    /* Reserve completion slots for the fresh cohort BEFORE any old handler can
     * finish, or all_done fires mid-rotation and start_pool tears the pool
     * down under us. */
    st->pending += workers;

    /* Workers notice the bump on their next tick and retire themselves. */
    zend_atomic_int_inc(&shared->epoch);

    /* Rotate: suspends until every old worker exited; the engine spawns the
     * replacement threads 1:1, each re-running the bootloader. */
    server->worker_pool->reload(server->worker_pool);

    if (UNEXPECTED(EG(exception))) {
        /* Rotation aborted (cancellation) — drop the reservation and the fresh
         * shells; a follow-up reload() builds new ones and heals the cohort. */
        st->pending -= workers;

        if (st->pending == 0 && st->all_done != NULL) {
            ZEND_ASYNC_CALLBACKS_NOTIFY(st->all_done, NULL, NULL);
        }

        for (int i = 0; i < workers; i++) {
            http_server_release_worker_shell(&fresh[i].server_transit);
        }

        pefree(fresh, 1);
        http_logf_info(&server->log_state, "reload.failed duration_ms=%lu",
                       (unsigned long)(ZEND_ASYNC_NOW() - t0));
        server->reload_in_progress = false;
        RETURN_THROWS();
    }

    /* Replacement threads are parked on the new task channel — hand each one
     * its start() task from the fresh shells. */
    int submitted = 0;

    for (int i = 0; i < workers; i++) {
        zend_async_event_t *worker_evt =
            server->worker_pool->submit_internal(server->worker_pool,
                                                 pool_worker_handler, &fresh[i]);

        if (UNEXPECTED(worker_evt == NULL)) {
            if (EG(exception)) {
                zend_clear_exception();
            }

            st->pending--;
            http_logf_info(&server->log_state, "reload.degraded worker=%d of %d",
                           i + 1, workers);
            continue;
        }

        zend_async_callbacks_push(worker_evt, &st->cb);
        fresh[i].done_event = worker_evt;
        submitted++;
    }

    if (st->pending == 0 && st->all_done != NULL) {
        ZEND_ASYNC_CALLBACKS_NOTIFY(st->all_done, NULL, NULL);
    }

    /* The old cohort has exited: its consumed shells can be released now. The
     * fresh array takes their slot on the server (http_server_free releases
     * whichever array is current at destruction). */
    pool_worker_ctx_t *old_ctxs = server->pool_worker_ctx;

    for (int i = 0; i < workers; i++) {
        /* Dispose the retired cohort's completion futures per rotation, so they
         * don't accumulate on the parent reactor across reloads. */
        if (old_ctxs[i].done_event != NULL) {
            ZEND_ASYNC_EVENT_RELEASE(old_ctxs[i].done_event);
            old_ctxs[i].done_event = NULL;
        }

        http_server_release_worker_shell(&old_ctxs[i].server_transit);
    }

    pefree(old_ctxs, 1);
    server->pool_worker_ctx = fresh;

    http_logf_info(&server->log_state, "reload.done workers=%d/%d duration_ms=%lu",
                   submitted, workers, (unsigned long)(ZEND_ASYNC_NOW() - t0));
    server->reload_in_progress = false;
    RETURN_BOOL(submitted == workers);
}
/* }}} */

/* ==========================================================================
 * Hot-reload triggers (issue #93) — pool-parent orchestrators.
 * ========================================================================== */

typedef struct {
    zend_object *server_obj;   /* addref'd parent HttpServer */
    zval         watcher;      /* owned Async\FileSystemWatcher */
} hot_reload_watch_ctx_t;

typedef struct {
    zend_object *server_obj;
} hot_reload_sighup_ctx_t;

static void hot_reload_watch_ctx_dispose(zend_coroutine_t *co)
{
    hot_reload_watch_ctx_t *ctx = co->extended_data;

    if (ctx == NULL) {
        return;
    }

    co->extended_data = NULL;
    zval_ptr_dtor(&ctx->watcher);
    OBJ_RELEASE(ctx->server_obj);
    efree(ctx);
}

static void hot_reload_sighup_ctx_dispose(zend_coroutine_t *co)
{
    hot_reload_sighup_ctx_t *ctx = co->extended_data;

    if (ctx == NULL) {
        return;
    }

    co->extended_data = NULL;
    OBJ_RELEASE(ctx->server_obj);
    efree(ctx);
}

#ifndef PHP_WIN32
/* Case-insensitive extension allow-list check; NULL/empty list = every file. */
static bool hot_reload_ext_matches(const HashTable *exts, const char *name)
{
    if (exts == NULL || zend_hash_num_elements((HashTable *)exts) == 0) {
        return true;
    }

    const char *dot = strrchr(name, '.');

    if (dot == NULL || dot[1] == '\0') {
        return false;
    }

    const zval *entry;
    ZEND_HASH_FOREACH_VAL((HashTable *)exts, entry)
    {
        if (Z_TYPE_P(entry) != IS_STRING) {
            continue;
        }

        const char *e = Z_STRVAL_P(entry);

        if (e[0] == '.') {
            e++;
        }

        if (strcasecmp(dot + 1, e) == 0) {
            return true;
        }
    }
    ZEND_HASH_FOREACH_END();

    return false;
}

/* opcache_invalidate($file, true) for every matching file under `dir`.
 * `fname` is the pre-resolved "opcache_invalidate" callable zval. */
static void hot_reload_invalidate_tree(const char *dir, const HashTable *exts,
                                       zval *fname, const int depth)
{
    if (depth > 16) {
        return;
    }

    DIR *d = opendir(dir);

    if (d == NULL) {
        return;
    }

    const struct dirent *ent;

    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.'
            && (ent->d_name[1] == '\0' || (ent->d_name[1] == '.' && ent->d_name[2] == '\0'))) {
            continue;
        }

        char full[MAXPATHLEN];

        if ((size_t)snprintf(full, sizeof(full), "%s/%s", dir, ent->d_name) >= sizeof(full)) {
            continue;
        }

        struct stat st;

        if (lstat(full, &st) != 0) {
            continue;
        }

        if (S_ISDIR(st.st_mode)) {
            hot_reload_invalidate_tree(full, exts, fname, depth + 1);
            continue;
        }

        if (!S_ISREG(st.st_mode) || !hot_reload_ext_matches(exts, ent->d_name)) {
            continue;
        }

        zval rv, params[2];
        ZVAL_UNDEF(&rv);
        ZVAL_STRING(&params[0], full);
        ZVAL_TRUE(&params[1]);
        call_user_function(NULL, NULL, fname, &rv, 2, params);
        zval_ptr_dtor(&params[0]);
        zval_ptr_dtor(&rv);

        if (EG(exception)) {
            zend_clear_exception();
        }
    }

    closedir(d);
}
#endif /* !PHP_WIN32 */

/* One trigger firing: invalidate the watched trees in opcache (so replacement
 * bootloaders recompile the changed files) and rotate via HttpServer::reload(). */
static void http_server_hot_reload_fire(zend_object *server_obj, const char *trigger)
{
    http_server_object *server = http_server_from_obj(server_obj);

    if (server->hot_reload_stopping || !server->in_pool_mode) {
        return;
    }

    http_logf_info(&server->log_state, "reload.trigger source=%s", trigger);

#ifndef PHP_WIN32
    http_server_config_t *cfg = http_server_get_config(server);

    if (cfg != NULL && Z_TYPE(cfg->hot_reload_paths) == IS_ARRAY) {
        zval fname;
        ZVAL_STRING(&fname, "opcache_invalidate");

        if (zend_is_callable(&fname, 0, NULL)) {
            const HashTable *exts = (Z_TYPE(cfg->hot_reload_extensions) == IS_ARRAY)
                                  ? Z_ARRVAL(cfg->hot_reload_extensions) : NULL;
            const zval *path;
            ZEND_HASH_FOREACH_VAL(Z_ARRVAL(cfg->hot_reload_paths), path)
            {
                hot_reload_invalidate_tree(Z_STRVAL_P(path), exts, &fname, 0);
            }
            ZEND_HASH_FOREACH_END();
        }

        zval_ptr_dtor(&fname);
    }
#endif

    zval rv;
    ZVAL_UNDEF(&rv);
    zend_call_method_with_0_params(server_obj, NULL, NULL, "reload", &rv);
    zval_ptr_dtor(&rv);

    if (EG(exception)) {
        http_logf_info(&server->log_state, "reload.trigger source=%s failed", trigger);
        zend_clear_exception();
    }
}

/* Coroutine: drive one watcher's iterator; every delivered (debounced) event
 * is one reload. Ends when the watcher is closed by teardown. */
static void http_server_hot_reload_watch_entry(void)
{
    zend_coroutine_t *co = ZEND_ASYNC_CURRENT_COROUTINE;
    hot_reload_watch_ctx_t *ctx = co->extended_data;
    http_server_object *server = http_server_from_obj(ctx->server_obj);

    zval iter;
    ZVAL_UNDEF(&iter);
    zend_call_method_with_0_params(Z_OBJ(ctx->watcher), NULL, NULL, "getIterator", &iter);

    if (EG(exception) || Z_TYPE(iter) != IS_OBJECT) {
        zend_clear_exception();
        zval_ptr_dtor(&iter);
        return;
    }

    zval rv;
    ZVAL_UNDEF(&rv);
    zend_call_method_with_0_params(Z_OBJ(iter), NULL, NULL, "rewind", &rv);
    zval_ptr_dtor(&rv);

    while (!EG(exception) && !server->hot_reload_stopping) {
        zval valid;
        ZVAL_UNDEF(&valid);
        zend_call_method_with_0_params(Z_OBJ(iter), NULL, NULL, "valid", &valid);

        const bool go = !EG(exception) && zend_is_true(&valid);
        zval_ptr_dtor(&valid);

        if (!go || server->hot_reload_stopping) {
            break;
        }

        http_server_hot_reload_fire(ctx->server_obj, "watcher");

        if (server->hot_reload_stopping) {
            break;
        }

        ZVAL_UNDEF(&rv);
        zend_call_method_with_0_params(Z_OBJ(iter), NULL, NULL, "next", &rv);
        zval_ptr_dtor(&rv);
    }

    if (EG(exception)) {
        zend_clear_exception();
    }

    zval_ptr_dtor(&iter);
}

#ifndef PHP_WIN32
/* Coroutine: persistent SIGHUP handler — one signal event armed for the whole
 * pool run (no unarmed window), each delivery is one reload. */
static void http_server_hot_reload_sighup_entry(void)
{
    zend_coroutine_t *co = ZEND_ASYNC_CURRENT_COROUTINE;
    hot_reload_sighup_ctx_t *ctx = co->extended_data;
    http_server_object *server = http_server_from_obj(ctx->server_obj);

    zend_async_signal_event_t *sig = ZEND_ASYNC_NEW_SIGNAL_EVENT(SIGHUP);

    if (UNEXPECTED(sig == NULL)) {
        zend_clear_exception();
        return;
    }

    if (UNEXPECTED(!sig->base.start(&sig->base))) {
        zend_clear_exception();
        sig->base.dispose(&sig->base);
        return;
    }

    server->sighup_event = sig;
    http_logf_info(&server->log_state, "reload.signal armed signal=SIGHUP");

    while (!server->hot_reload_stopping) {
        zend_async_resume_when(co, &sig->base, false, zend_async_waker_callback_resolve, NULL);
        ZEND_ASYNC_SUSPEND();
        ZEND_ASYNC_WAKER_DESTROY(co);

        if (EG(exception)) {
            zend_clear_exception();
            break;
        }

        if (server->hot_reload_stopping) {
            break;
        }

        http_server_hot_reload_fire(ctx->server_obj, "sighup");
    }

    server->sighup_event = NULL;
    sig->base.stop(&sig->base);
    sig->base.dispose(&sig->base);
}
#endif /* !PHP_WIN32 */

/* Arm the configured triggers on the pool parent (called from start_pool just
 * before it suspends). Failures degrade to "trigger off" with a log line. */
static void http_server_hot_reload_up(http_server_object *server, zval *this_zv,
                                      http_server_config_t *cfg)
{
    server->hot_reload_stopping = false;

    if (Z_TYPE(cfg->hot_reload_paths) == IS_ARRAY) {
        zend_string *cls = zend_string_init("Async\\FileSystemWatcher",
                                            sizeof("Async\\FileSystemWatcher") - 1, 0);
        zend_class_entry *ce = zend_lookup_class(cls);
        zend_string_release(cls);

        if (UNEXPECTED(ce == NULL)) {
            zend_clear_exception();
            http_logf_info(&server->log_state,
                           "reload.watch unavailable (Async\\FileSystemWatcher missing)");
            return;
        }

        array_init(&server->hot_reload_watchers);

        const zval *path;
        ZEND_HASH_FOREACH_VAL(Z_ARRVAL(cfg->hot_reload_paths), path)
        {
            zval watcher;

            if (object_init_ex(&watcher, ce) != SUCCESS) {
                zend_clear_exception();
                continue;
            }

            zval args[6], rv;
            ZVAL_COPY(&args[0], path);
            ZVAL_TRUE(&args[1]);                                /* recursive */
            ZVAL_TRUE(&args[2]);                                /* coalesce  */
            ZVAL_LONG(&args[3], cfg->hot_reload_debounce_ms);
            ZVAL_LONG(&args[4], cfg->hot_reload_max_hold_ms);

            if (Z_TYPE(cfg->hot_reload_extensions) == IS_ARRAY) {
                ZVAL_COPY(&args[5], &cfg->hot_reload_extensions);
            } else {
                array_init(&args[5]);
            }

            ZVAL_UNDEF(&rv);
            zend_call_known_function(ce->constructor, Z_OBJ(watcher), ce, &rv,
                                     6, args, NULL);
            zval_ptr_dtor(&rv);
            zval_ptr_dtor(&args[0]);
            zval_ptr_dtor(&args[5]);

            if (EG(exception)) {
                http_logf_info(&server->log_state, "reload.watch failed path=%s",
                               Z_STRVAL_P(path));
                zend_clear_exception();
                zval_ptr_dtor(&watcher);
                continue;
            }

            hot_reload_watch_ctx_t *wctx = emalloc(sizeof(*wctx));
            wctx->server_obj = Z_OBJ_P(this_zv);
            GC_ADDREF(wctx->server_obj);
            ZVAL_COPY(&wctx->watcher, &watcher);

            zend_coroutine_t *co = ZEND_ASYNC_NEW_COROUTINE(ZEND_ASYNC_MAIN_SCOPE);

            if (UNEXPECTED(co == NULL)) {
                zend_clear_exception();
                zval_ptr_dtor(&wctx->watcher);
                OBJ_RELEASE(wctx->server_obj);
                efree(wctx);
                zval_ptr_dtor(&watcher);
                continue;
            }

            co->internal_entry   = http_server_hot_reload_watch_entry;
            co->extended_data    = wctx;
            co->extended_dispose = hot_reload_watch_ctx_dispose;
            ZEND_ASYNC_ENQUEUE_COROUTINE(co);

            add_next_index_zval(&server->hot_reload_watchers, &watcher);
            http_logf_info(&server->log_state,
                           "reload.watch armed path=%s debounce_ms=%u max_hold_ms=%u",
                           Z_STRVAL_P(path), cfg->hot_reload_debounce_ms,
                           cfg->hot_reload_max_hold_ms);
        }
        ZEND_HASH_FOREACH_END();
    }

#ifndef PHP_WIN32
    if (cfg->reload_on_sighup) {
        hot_reload_sighup_ctx_t *sctx = emalloc(sizeof(*sctx));
        sctx->server_obj = Z_OBJ_P(this_zv);
        GC_ADDREF(sctx->server_obj);

        zend_coroutine_t *co = ZEND_ASYNC_NEW_COROUTINE(ZEND_ASYNC_MAIN_SCOPE);

        if (UNEXPECTED(co == NULL)) {
            zend_clear_exception();
            OBJ_RELEASE(sctx->server_obj);
            efree(sctx);
            return;
        }

        co->internal_entry   = http_server_hot_reload_sighup_entry;
        co->extended_data    = sctx;
        co->extended_dispose = hot_reload_sighup_ctx_dispose;
        ZEND_ASYNC_ENQUEUE_COROUTINE(co);
    }
#endif
}

/* Retire the triggers: closing a watcher ends its iterator (the orchestrator
 * sees valid()=false); notifying the signal event wakes its waiter, which
 * observes hot_reload_stopping and disposes the event itself. */
static void http_server_hot_reload_down(http_server_object *server)
{
    server->hot_reload_stopping = true;

    if (Z_TYPE(server->hot_reload_watchers) == IS_ARRAY) {
        const zval *w;
        ZEND_HASH_FOREACH_VAL(Z_ARRVAL(server->hot_reload_watchers), w)
        {
            zval rv;
            ZVAL_UNDEF(&rv);
            zend_call_method_with_0_params(Z_OBJ_P(w), NULL, NULL, "close", &rv);
            zval_ptr_dtor(&rv);

            if (EG(exception)) {
                zend_clear_exception();
            }
        }
        ZEND_HASH_FOREACH_END();

        zval_ptr_dtor(&server->hot_reload_watchers);
        ZVAL_UNDEF(&server->hot_reload_watchers);
    }

    if (server->sighup_event != NULL) {
        ZEND_ASYNC_CALLBACKS_NOTIFY((zend_async_event_t *) server->sighup_event, NULL, NULL);
    }
}

/* {{{ proto HttpServer::isRunning(): bool */
ZEND_METHOD(TrueAsync_HttpServer, isRunning)
{
    ZEND_PARSE_PARAMETERS_NONE();

    http_server_object *server = Z_HTTP_SERVER_P(ZEND_THIS);
    RETURN_BOOL(server->running);
}
/* }}} */

/* {{{ proto HttpServer::isHttp2(): bool — compile-time --enable-http2 probe */
ZEND_METHOD(TrueAsync_HttpServer, isHttp2)
{
    ZEND_PARSE_PARAMETERS_NONE();
#ifdef HAVE_HTTP2
    RETURN_TRUE;
#else
    RETURN_FALSE;
#endif
}
/* }}} */

/* {{{ proto HttpServer::isHttp3(): bool — compile-time --enable-http3 probe */
ZEND_METHOD(TrueAsync_HttpServer, isHttp3)
{
    ZEND_PARSE_PARAMETERS_NONE();
#ifdef HAVE_HTTP_SERVER_HTTP3
    RETURN_TRUE;
#else
    RETURN_FALSE;
#endif
}
/* }}} */

/* {{{ proto HttpServer::getTelemetry(): array */
ZEND_METHOD(TrueAsync_HttpServer, getTelemetry)
{
    ZEND_PARSE_PARAMETERS_NONE();

    http_server_object *server = Z_HTTP_SERVER_P(ZEND_THIS);

    array_init(return_value);
    add_assoc_long(return_value, "total_requests", (zend_long)server->counters_live->total_requests);
    add_assoc_long(return_value, "active_connections", server->active_connections);
    add_assoc_long(return_value, "active_requests", (zend_long)server->counters_live->active_requests);
    add_assoc_long(return_value, "max_inflight_requests", (zend_long)server->max_inflight_requests);
    add_assoc_long(return_value, "requests_shed_total", (zend_long)server->counters_live->requests_shed_total);
    add_assoc_long(return_value, "bytes_received", 0);  /* TODO */
    add_assoc_long(return_value, "bytes_sent", 0);      /* TODO */
    add_assoc_long(return_value, "errors", 0);          /* TODO */

    /* Backpressure telemetry. All durations exposed in milliseconds
     * (float) so PHP callers can graph without extra conversion. */
    const double ns_to_ms = 1.0 / 1000000.0;
    add_assoc_bool (return_value, "listeners_paused",     server->listeners_paused);
    add_assoc_long (return_value, "pause_high",           (zend_long)server->pause_high);
    add_assoc_long (return_value, "pause_low",            (zend_long)server->pause_low);
    add_assoc_long (return_value, "pause_count_total",    (zend_long)server->pause_count_total);
    add_assoc_long (return_value, "codel_trips_total",    (zend_long)server->codel_trips_total);
    add_assoc_double(return_value, "paused_total_ms",
                     (double)server->paused_total_ns * ns_to_ms);
    add_assoc_long (return_value, "sojourn_samples",      (zend_long)server->sojourn_samples);
    add_assoc_double(return_value, "sojourn_avg_ms",
                     server->sojourn_samples
                       ? (double)server->sojourn_sum_ns / (double)server->sojourn_samples * ns_to_ms
                       : 0.0);
    add_assoc_double(return_value, "sojourn_max_ms",
                     (double)server->sojourn_max_ns * ns_to_ms);
    add_assoc_double(return_value, "service_avg_ms",
                     server->sojourn_samples
                       ? (double)server->service_sum_ns / (double)server->sojourn_samples * ns_to_ms
                       : 0.0);

    /* TLS telemetry. Zero-valued on builds without OpenSSL (fields are
     * always present for a stable ABI to PHP callers / dashboards). */
    add_assoc_long  (return_value, "tls_handshakes_total",           (zend_long)server->tls_handshakes_total);
    add_assoc_long  (return_value, "tls_handshake_failures_total",   (zend_long)server->tls_handshake_failures_total);
    add_assoc_double(return_value, "tls_handshake_avg_ms",
                     server->tls_handshake_ns_count
                       ? (double)server->tls_handshake_ns_sum / (double)server->tls_handshake_ns_count * ns_to_ms
                       : 0.0);
    add_assoc_long  (return_value, "tls_resumed_total",              (zend_long)server->tls_resumed_total);
    add_assoc_long  (return_value, "tls_bytes_plaintext_in_total",   (zend_long)server->counters_live->tls_bytes_plaintext_in_total);
    add_assoc_long  (return_value, "tls_bytes_plaintext_out_total",  (zend_long)server->counters_live->tls_bytes_plaintext_out_total);
    add_assoc_long  (return_value, "tls_bytes_ciphertext_in_total",  (zend_long)server->counters_live->tls_bytes_ciphertext_in_total);
    add_assoc_long  (return_value, "tls_bytes_ciphertext_out_total", (zend_long)server->counters_live->tls_bytes_ciphertext_out_total);
    add_assoc_long  (return_value, "tls_ktls_tx_total",              (zend_long)server->tls_ktls_tx_total);
    add_assoc_long  (return_value, "tls_ktls_rx_total",              (zend_long)server->tls_ktls_rx_total);

    /* Parser-error counters. Always present
     * even if zero so dashboards have a stable schema. */
    add_assoc_long  (return_value, "parse_errors_4xx_total", (zend_long)server->parse_errors_4xx_total);
    add_assoc_long  (return_value, "parse_errors_400_total", (zend_long)server->parse_errors_400_total);
    add_assoc_long  (return_value, "parse_errors_413_total", (zend_long)server->parse_errors_413_total);
    add_assoc_long  (return_value, "parse_errors_414_total", (zend_long)server->parse_errors_414_total);
    add_assoc_long  (return_value, "parse_errors_431_total", (zend_long)server->parse_errors_431_total);
    add_assoc_long  (return_value, "parse_errors_503_total", (zend_long)server->parse_errors_503_total);

    /* Connection-drain telemetry. */
    add_assoc_long(return_value, "drain_epoch_current",
                   (zend_long)server->view.drain_epoch_current);
    add_assoc_long(return_value, "drain_events_reactive_total",
                   (zend_long)server->drain_events_reactive_total);
    add_assoc_long(return_value, "drain_events_cooldown_blocked_total",
                   (zend_long)server->drain_events_cooldown_blocked_total);
    add_assoc_long(return_value, "connections_drained_reactive_total",
                   (zend_long)server->connections_drained_reactive_total);
    add_assoc_long(return_value, "connections_drained_proactive_total",
                   (zend_long)server->connections_drained_proactive_total);
    add_assoc_long(return_value, "h2_goaway_sent_total",
                   (zend_long)server->counters_live->h2_goaway_sent_total);
    add_assoc_long(return_value, "h3_goaway_sent_total",
                   (zend_long)server->counters_live->h3_goaway_sent_total);
    add_assoc_long(return_value, "h1_connection_close_sent_total",
                   (zend_long)server->counters_live->h1_connection_close_sent_total);
    add_assoc_long(return_value, "connections_force_closed_total",
                   (zend_long)server->connections_force_closed_total);

    /* Streaming-response telemetry. */
    add_assoc_long(return_value, "streaming_responses_total",
                   (zend_long)server->counters_live->streaming_responses_total);
    add_assoc_long(return_value, "stream_send_calls_total",
                   (zend_long)server->counters_live->stream_send_calls_total);
    add_assoc_long(return_value, "stream_send_backpressure_events_total",
                   (zend_long)server->counters_live->stream_send_backpressure_events_total);
    add_assoc_long(return_value, "stream_bytes_sent_total",
                   (zend_long)server->counters_live->stream_bytes_sent_total);

    /* StaticHandler hard-zero hit counter (issue #13). */
    add_assoc_long(return_value, "static_zero_coroutine_total",
                   (zend_long)server->counters_live->static_zero_coroutine_total);
    add_assoc_long(return_value, "static_cache_hits_total",
                   (zend_long)server->counters_live->static_cache_hits_total);
    add_assoc_long(return_value, "static_cache_misses_total",
                   (zend_long)server->counters_live->static_cache_misses_total);

    /* HTTP/2 stream-level telemetry. */
    add_assoc_long(return_value, "h2_streams_active",
                   (zend_long)server->counters_live->h2_streams_active);
    add_assoc_long(return_value, "h2_streams_opened_total",
                   (zend_long)server->counters_live->h2_streams_opened_total);
    add_assoc_long(return_value, "h2_streams_reset_by_peer_total",
                   (zend_long)server->counters_live->h2_streams_reset_by_peer_total);
    add_assoc_long(return_value, "h2_streams_refused_total",
                   (zend_long)server->counters_live->h2_streams_refused_total);
    add_assoc_long(return_value, "h2_goaway_recv_total",
                   (zend_long)server->counters_live->h2_goaway_recv_total);
    add_assoc_long(return_value, "h2_data_recv_bytes_total",
                   (zend_long)server->counters_live->h2_data_recv_bytes_total);
    add_assoc_long(return_value, "h2_data_sent_bytes_total",
                   (zend_long)server->counters_live->h2_data_sent_bytes_total);
    add_assoc_long(return_value, "h2_ping_rtt_ns",
                   (zend_long)server->counters_live->h2_ping_rtt_ns);
}
/* }}} */

/* {{{ proto HttpServer::resetTelemetry(): bool */
ZEND_METHOD(TrueAsync_HttpServer, resetTelemetry)
{
    ZEND_PARSE_PARAMETERS_NONE();

    http_server_object *server = Z_HTTP_SERVER_P(ZEND_THIS);

    server->counters_live->total_requests = 0;
    server->counters_live->static_zero_coroutine_total = 0;
    server->counters_live->static_cache_hits_total     = 0;
    server->counters_live->static_cache_misses_total   = 0;
    server->sojourn_sum_ns       = 0;
    server->service_sum_ns       = 0;
    server->sojourn_max_ns       = 0;
    server->sojourn_samples      = 0;
    server->pause_count_total    = 0;
    server->codel_trips_total    = 0;
    server->paused_total_ns      = 0;
    server->tls_handshakes_total           = 0;
    server->tls_handshake_failures_total   = 0;
    server->tls_handshake_ns_sum           = 0;
    server->tls_handshake_ns_count         = 0;
    server->tls_resumed_total              = 0;
    server->counters_live->tls_bytes_plaintext_in_total   = 0;
    server->counters_live->tls_bytes_plaintext_out_total  = 0;
    server->counters_live->tls_bytes_ciphertext_in_total  = 0;
    server->counters_live->tls_bytes_ciphertext_out_total = 0;
    server->tls_ktls_tx_total              = 0;
    server->tls_ktls_rx_total              = 0;
    server->parse_errors_4xx_total         = 0;
    server->parse_errors_400_total         = 0;
    server->parse_errors_413_total         = 0;
    server->parse_errors_414_total         = 0;
    server->parse_errors_431_total         = 0;
    server->parse_errors_503_total         = 0;
    server->counters_live->requests_shed_total            = 0;
    server->counters_live->h2_streams_refused_total       = 0;

    /* Drain counters. drain_epoch_current / drain_last_fired_ns are
     * runtime state, NOT cleared (same rationale as paused_since_ns). */
    server->connections_drained_reactive_total   = 0;
    server->connections_drained_proactive_total  = 0;
    server->counters_live->h2_goaway_sent_total                 = 0;
    server->counters_live->h3_goaway_sent_total                 = 0;
    server->counters_live->h1_connection_close_sent_total       = 0;
    server->connections_force_closed_total       = 0;
    server->drain_events_reactive_total          = 0;
    server->drain_events_cooldown_blocked_total  = 0;
    /* Streaming counters. */
    memset(server->counters_live, 0, sizeof(*server->counters_live));
    /* HTTP/2 stream telemetry. Active count is
     * live state — NOT cleared (otherwise operators would see a
     * negative drift as close events decrement past zero). */
    server->counters_live->h2_streams_opened_total                = 0;
    server->counters_live->h2_streams_reset_by_peer_total         = 0;
    server->counters_live->h2_goaway_recv_total                   = 0;
    server->counters_live->h2_data_recv_bytes_total               = 0;
    server->counters_live->h2_data_sent_bytes_total               = 0;
    server->counters_live->h2_ping_rtt_ns                         = 0;
    /* Don't reset paused_since_ns or CoDel runtime state — those track
     * live pause and would confuse the control loop if cleared mid-flight. */

    RETURN_TRUE;
}
/* }}} */

/* Emit one counters slice as assoc entries (issue #5, A4). Shared by every
 * worker entry and the summed totals block. */
static void stats_counters_to_zval(zval *arr, const http_server_counters_t *c)
{
    add_assoc_long(arr, "total_requests",         (zend_long)c->total_requests);
    add_assoc_long(arr, "active_requests",        (zend_long)c->active_requests);
    add_assoc_long(arr, "requests_shed_total",    (zend_long)c->requests_shed_total);
    add_assoc_long(arr, "responses_2xx_total",    (zend_long)c->responses_2xx_total);
    add_assoc_long(arr, "responses_3xx_total",    (zend_long)c->responses_3xx_total);
    add_assoc_long(arr, "responses_4xx_total",    (zend_long)c->responses_4xx_total);
    add_assoc_long(arr, "responses_5xx_total",    (zend_long)c->responses_5xx_total);
    add_assoc_long(arr, "conns_active_h1",        (zend_long)c->conns_active_h1);
    add_assoc_long(arr, "conns_active_h2",        (zend_long)c->conns_active_h2);
    add_assoc_long(arr, "conns_active_h3",        (zend_long)c->conns_active_h3);
    add_assoc_long(arr, "streaming_responses_total",             (zend_long)c->streaming_responses_total);
    add_assoc_long(arr, "stream_send_calls_total",               (zend_long)c->stream_send_calls_total);
    add_assoc_long(arr, "stream_bytes_sent_total",               (zend_long)c->stream_bytes_sent_total);
    add_assoc_long(arr, "stream_send_backpressure_events_total", (zend_long)c->stream_send_backpressure_events_total);
    add_assoc_long(arr, "worker_wire_dropped_total",             (zend_long)c->worker_wire_dropped_total);
    add_assoc_long(arr, "h2_streams_active",       (zend_long)c->h2_streams_active);
    add_assoc_long(arr, "h2_streams_opened_total", (zend_long)c->h2_streams_opened_total);
    add_assoc_long(arr, "h2_streams_reset_by_peer_total", (zend_long)c->h2_streams_reset_by_peer_total);
    add_assoc_long(arr, "h2_streams_refused_total",(zend_long)c->h2_streams_refused_total);
    add_assoc_long(arr, "h2_goaway_recv_total",    (zend_long)c->h2_goaway_recv_total);
    add_assoc_long(arr, "h2_goaway_sent_total",    (zend_long)c->h2_goaway_sent_total);
    add_assoc_long(arr, "h2_data_recv_bytes_total",(zend_long)c->h2_data_recv_bytes_total);
    add_assoc_long(arr, "h2_data_sent_bytes_total",(zend_long)c->h2_data_sent_bytes_total);
    add_assoc_long(arr, "h1_connection_close_sent_total", (zend_long)c->h1_connection_close_sent_total);
    add_assoc_long(arr, "h3_goaway_sent_total",    (zend_long)c->h3_goaway_sent_total);
    add_assoc_long(arr, "tls_bytes_plaintext_in_total",   (zend_long)c->tls_bytes_plaintext_in_total);
    add_assoc_long(arr, "tls_bytes_plaintext_out_total",  (zend_long)c->tls_bytes_plaintext_out_total);
    add_assoc_long(arr, "tls_bytes_ciphertext_in_total",  (zend_long)c->tls_bytes_ciphertext_in_total);
    add_assoc_long(arr, "tls_bytes_ciphertext_out_total", (zend_long)c->tls_bytes_ciphertext_out_total);
    add_assoc_long(arr, "static_zero_coroutine_total",    (zend_long)c->static_zero_coroutine_total);
    add_assoc_long(arr, "static_cache_hits_total",   (zend_long)c->static_cache_hits_total);
    add_assoc_long(arr, "static_cache_misses_total", (zend_long)c->static_cache_misses_total);
}

/* Field-wise accumulate into the totals block. The slice is a POD of 64-bit
 * counters (every field is uint64_t or size_t == 8 bytes on our targets), so a
 * word-wise add covers every field and stays correct when A5 adds more. */
static void stats_counters_add(http_server_counters_t *acc, const http_server_counters_t *c)
{
    uint64_t       *dst = (uint64_t *)acc;
    const uint64_t *src = (const uint64_t *)c;

    for (size_t i = 0; i < sizeof(*acc) / sizeof(uint64_t); i++) {
        dst[i] += src[i];
    }
}

/* {{{ proto HttpServer::getStats(): array
 *
 * Cross-worker statistics aggregate (issue #5). Opt-in: throws when
 * setStatsEnabled(true) was not set. Returns
 *   { enabled: true, workers: { <id>: {..counters..}, ... }, totals: {..} }
 * In pool mode it walks the shared slab, reading each worker's slot lock-free
 * (no CAS); a slot mid-retire is skipped, so the aggregate can be stale by one
 * worker — acceptable for statistics. A single-worker server reports its own
 * counters as the sole worker. */
ZEND_METHOD(TrueAsync_HttpServer, getStats)
{
    ZEND_PARSE_PARAMETERS_NONE();

    http_server_object *const server = Z_HTTP_SERVER_P(ZEND_THIS);
    const http_server_config_t *const cfg = http_server_get_config(server);

    if (cfg == NULL || !cfg->stats_enabled) {
        zend_throw_exception(http_server_runtime_exception_ce,
            "Statistics are not enabled — call HttpServerConfig::setStatsEnabled(true)", 0);
        RETURN_THROWS();
    }

    http_server_counters_t totals;
    memset(&totals, 0, sizeof(totals));

    zval workers;
    array_init(&workers);

    if (g_stats_registry != NULL) {
        const int cap = http_stats_registry_capacity(g_stats_registry);

        for (int i = 0; i < cap; i++) {
            const http_stats_slot_t *const slot = http_stats_registry_at(g_stats_registry, i);

            if (!http_stats_slot_active(slot)) {
                continue;
            }

            zval w;
            array_init(&w);
            stats_counters_to_zval(&w, &slot->counters);
            add_index_zval(&workers, (zend_long)slot->worker_id, &w);
            stats_counters_add(&totals, &slot->counters);
        }
    } else {
        /* Single-worker / non-pool: the server's own counters are the one slot. */
        zval w;
        array_init(&w);
        stats_counters_to_zval(&w, server->counters_live);
        add_index_zval(&workers, 0, &w);
        stats_counters_add(&totals, server->counters_live);
    }

    array_init(return_value);
    add_assoc_bool(return_value, "enabled", true);
    add_assoc_zval(return_value, "workers", &workers);

    zval totals_zv;
    array_init(&totals_zv);
    stats_counters_to_zval(&totals_zv, &totals);
    add_assoc_zval(return_value, "totals", &totals_zv);
}
/* }}} */

/* {{{ proto HttpServer::getConfig(): HttpServerConfig */
ZEND_METHOD(TrueAsync_HttpServer, getConfig)
{
    ZEND_PARSE_PARAMETERS_NONE();

    http_server_object *server = Z_HTTP_SERVER_P(ZEND_THIS);

    RETURN_ZVAL(&server->config, 1, 0);
}
/* }}} */

#ifdef HAVE_HTTP_SERVER_HTTP3
/* Append one listener's stats snapshot to the result array. Factored out so
 * both the single-thread listeners (server->http3_listeners) and the reactor-
 * owned listeners (server->reactor_h3_listeners) report identically.
 * The reactor-owned read is cross-thread (the reactor writes these counters on
 * its own thread); they are advisory uint64s, so a torn read is benign. */
static void http3_emit_listener_stats(zval *return_value, http3_listener_t *listener)
{
    http3_listener_stats_t s;
    http3_listener_get_stats(listener, &s);

    zval entry;
    array_init(&entry);
    add_assoc_string(&entry, "host", (char *)http3_listener_host(listener));
    add_assoc_long  (&entry, "port", http3_listener_port(listener));
    add_assoc_long  (&entry, "datagrams_received", (zend_long)s.datagrams_received);
    add_assoc_long  (&entry, "bytes_received",     (zend_long)s.bytes_received);
    add_assoc_long  (&entry, "datagrams_errored",  (zend_long)s.datagrams_errored);
    add_assoc_long  (&entry, "last_datagram_size", (zend_long)s.last_datagram_size);
    add_assoc_string(&entry, "last_peer",          s.last_peer);

    /* QUIC packet classification counters. */
    add_assoc_long(&entry, "quic_initial",            (zend_long)s.packet.quic_initial);
    add_assoc_long(&entry, "quic_short_header",       (zend_long)s.packet.quic_short_header);
    add_assoc_long(&entry, "quic_version_negotiated", (zend_long)s.packet.quic_version_negotiated);
    add_assoc_long(&entry, "quic_parse_errors",       (zend_long)s.packet.quic_parse_errors);
    /* ngtcp2_conn lifecycle counters. */
    add_assoc_long(&entry, "quic_conn_accepted",      (zend_long)s.packet.quic_conn_accepted);
    add_assoc_long(&entry, "quic_conn_rejected",      (zend_long)s.packet.quic_conn_rejected);
    /* Read-path counters. */
    add_assoc_long(&entry, "quic_read_ok",            (zend_long)s.packet.quic_read_ok);
    add_assoc_long(&entry, "quic_read_error",         (zend_long)s.packet.quic_read_error);
    add_assoc_long(&entry, "quic_read_fatal",         (zend_long)s.packet.quic_read_fatal);
    add_assoc_long(&entry, "quic_path_migrations",    (zend_long)s.packet.quic_path_migrations);
    add_assoc_long(&entry, "quic_migration_storm_shed", (zend_long)s.packet.quic_migration_storm_shed);
    /* CID steering. */
    add_assoc_long(&entry, "quic_steered_out",        (zend_long)s.packet.quic_steered_out);
    add_assoc_long(&entry, "quic_steered_in",         (zend_long)s.packet.quic_steered_in);
    add_assoc_long(&entry, "quic_steered_drop",       (zend_long)s.packet.quic_steered_drop);
    /* Issued / retired alternate CIDs (NEW_CONNECTION_ID, RFC 9000 §5.1). */
    add_assoc_long(&entry, "quic_new_cid_issued",     (zend_long)s.packet.quic_new_cid_issued);
    add_assoc_long(&entry, "quic_cid_retired",        (zend_long)s.packet.quic_cid_retired);
    /* Write-loop + timer counters. */
    add_assoc_long(&entry, "quic_packets_sent",       (zend_long)s.packet.quic_packets_sent);
    add_assoc_long(&entry, "quic_bytes_sent",         (zend_long)s.packet.quic_bytes_sent);
    add_assoc_long(&entry, "quic_timer_fired",        (zend_long)s.packet.quic_timer_fired);
    add_assoc_long(&entry, "quic_write_error",        (zend_long)s.packet.quic_write_error);
    /* Handshake / ALPN counters. */
    add_assoc_long(&entry, "quic_handshake_completed", (zend_long)s.packet.quic_handshake_completed);
    add_assoc_long(&entry, "quic_alpn_mismatch",       (zend_long)s.packet.quic_alpn_mismatch);
    /* nghttp3 lifecycle counters. */
    add_assoc_long(&entry, "h3_init_ok",            (zend_long)s.packet.h3_init_ok);
    add_assoc_long(&entry, "h3_init_failed",        (zend_long)s.packet.h3_init_failed);
    add_assoc_long(&entry, "h3_stream_close",       (zend_long)s.packet.h3_stream_close);
    add_assoc_long(&entry, "h3_stream_read_error",  (zend_long)s.packet.h3_stream_read_error);
    /* Request-assembly counters. */
    add_assoc_long(&entry, "h3_request_received",   (zend_long)s.packet.h3_request_received);
    add_assoc_long(&entry, "h3_request_oversized",  (zend_long)s.packet.h3_request_oversized);
    add_assoc_long(&entry, "h3_streams_opened",     (zend_long)s.packet.h3_streams_opened);
    /* Response counters. */
    add_assoc_long(&entry, "h3_response_submitted",   (zend_long)s.packet.h3_response_submitted);
    add_assoc_long(&entry, "h3_response_submit_error",(zend_long)s.packet.h3_response_submit_error);
    /* Connection lifecycle counters. */
    add_assoc_long(&entry, "quic_connection_close_sent", (zend_long)s.packet.quic_connection_close_sent);
    add_assoc_long(&entry, "quic_conn_in_closing",       (zend_long)s.packet.quic_conn_in_closing);
    add_assoc_long(&entry, "quic_conn_in_draining",      (zend_long)s.packet.quic_conn_in_draining);
    add_assoc_long(&entry, "quic_conn_idle_closed",      (zend_long)s.packet.quic_conn_idle_closed);
    add_assoc_long(&entry, "quic_conn_handshake_timeout",(zend_long)s.packet.quic_conn_handshake_timeout);
    add_assoc_long(&entry, "quic_conn_reaped",           (zend_long)s.packet.quic_conn_reaped);
    add_assoc_long(&entry, "quic_stateless_reset_sent",  (zend_long)s.packet.quic_stateless_reset_sent);
    add_assoc_long(&entry, "quic_retry_sent",            (zend_long)s.packet.quic_retry_sent);
    add_assoc_long(&entry, "quic_retry_token_ok",        (zend_long)s.packet.quic_retry_token_ok);
    add_assoc_long(&entry, "quic_retry_token_invalid",   (zend_long)s.packet.quic_retry_token_invalid);
    add_assoc_long(&entry, "quic_conn_per_peer_rejected",(zend_long)s.packet.quic_conn_per_peer_rejected);
    add_assoc_long(&entry, "quic_conn_global_rejected", (zend_long)s.packet.quic_conn_global_rejected);
    add_assoc_long(&entry, "quic_conn_refused_sent",    (zend_long)s.packet.quic_conn_refused_sent);
    /* Audit hardening counters. */
    add_assoc_long(&entry, "h3_framing_error",           (zend_long)s.packet.h3_framing_error);
    add_assoc_long(&entry, "quic_drain_iter_cap_hit",    (zend_long)s.packet.quic_drain_iter_cap_hit);

    /* Reactor-iteration watchdog. Tick = one poll-cb wakeup; on the single
     * reactor thread its latency is the ACK/PTO delay imposed on every live
     * connection. */
    add_assoc_long(&entry, "reactor_ticks",            (zend_long)s.packet.reactor_ticks);
    add_assoc_long(&entry, "reactor_busy_ns",          (zend_long)s.packet.reactor_busy_ns);
    add_assoc_long(&entry, "reactor_max_tick_ns",      (zend_long)s.packet.reactor_max_tick_ns);
    add_assoc_long(&entry, "reactor_slow_ticks",       (zend_long)s.packet.reactor_slow_ticks);
    add_assoc_long(&entry, "reactor_timer_late",       (zend_long)s.packet.reactor_timer_late);
    add_assoc_long(&entry, "reactor_max_timer_late_ns",(zend_long)s.packet.reactor_max_timer_late_ns);
    {
        zval hist;
        array_init(&hist);

        const size_t nbuckets = sizeof(s.packet.reactor_lat_bucket)
                              / sizeof(s.packet.reactor_lat_bucket[0]);

        for (size_t i = 0; i < nbuckets; ++i) {
            add_next_index_long(&hist, (zend_long)s.packet.reactor_lat_bucket[i]);
        }

        add_assoc_zval(&entry, "reactor_lat_bucket", &hist);
    }

    /* Send-path error categorisation. */
    add_assoc_long(&entry, "quic_send_eagain",           (zend_long)s.packet.quic_send_eagain);
    add_assoc_long(&entry, "quic_send_gso_refused",      (zend_long)s.packet.quic_send_gso_refused);
    add_assoc_long(&entry, "quic_send_emsgsize",         (zend_long)s.packet.quic_send_emsgsize);
    add_assoc_long(&entry, "quic_send_unreach",          (zend_long)s.packet.quic_send_unreach);
    add_assoc_long(&entry, "quic_send_other_error",      (zend_long)s.packet.quic_send_other_error);
    add_assoc_long(&entry, "quic_gso_disabled",          (zend_long)s.packet.quic_gso_disabled);

    /* Async errors observed via MSG_ERRQUEUE. */
    add_assoc_long(&entry, "quic_errqueue_emsgsize",     (zend_long)s.packet.quic_errqueue_emsgsize);
    add_assoc_long(&entry, "quic_errqueue_unreach",      (zend_long)s.packet.quic_errqueue_unreach);
    add_assoc_long(&entry, "quic_errqueue_other",        (zend_long)s.packet.quic_errqueue_other);

    add_next_index_zval(return_value, &entry);
}
#endif

/* {{{ proto HttpServer::getHttp3Stats(): array
 *
 * Per-listener observability for the HTTP/3 path. In single-thread / worker
 * mode the listeners live on this server; in the reactor-pool split the
 * transport reactors own them (server->reactor_h3_listeners) — report both so
 * a pooled server is observable too. Counters let tests confirm the UDP pipe
 * is live end-to-end. */
ZEND_METHOD(TrueAsync_HttpServer, getHttp3Stats)
{
    ZEND_PARSE_PARAMETERS_NONE();

    array_init(return_value);

#ifdef HAVE_HTTP_SERVER_HTTP3
    http_server_object *server = Z_HTTP_SERVER_P(ZEND_THIS);

    for (size_t i = 0; i < server->http3_listener_count; i++) {
        if (server->http3_listeners[i] != NULL) {
            http3_emit_listener_stats(return_value, server->http3_listeners[i]);
        }
    }

    /* Reactor-pool split: the transport reactors own the H3 listeners. */
    for (size_t i = 0; i < server->reactor_h3_listener_count; i++) {
        if (server->reactor_h3_listeners[i].listener != NULL) {
            http3_emit_listener_stats(return_value, server->reactor_h3_listeners[i].listener);
        }
    }
#endif
}
/* }}} */

/* {{{ HttpServer::getRuntimeStats — snapshot of internal allocators. */
ZEND_METHOD(TrueAsync_HttpServer, getRuntimeStats)
{
    ZEND_PARSE_PARAMETERS_NONE();

    http_server_object *server = Z_HTTP_SERVER_P(ZEND_THIS);

    array_init(return_value);

    conn_arena_stats_t cstat;
    conn_arena_get_stats(&server->conn_arena, &cstat);

    add_assoc_long(return_value, "conn_arena_live",   (zend_long)cstat.live);
    add_assoc_long(return_value, "conn_arena_slots",  (zend_long)cstat.slots);
    add_assoc_long(return_value, "conn_arena_chunks", (zend_long)cstat.chunks);
    add_assoc_long(return_value, "conn_arena_bytes",
                   (zend_long)(cstat.chunks * CONN_ARENA_CHUNK_SLOTS * cstat.slot_bytes));

    body_pool_class_stats_t pstats[BODY_POOL_NUM_CLASSES];
    body_pool_get_stats(pstats);

    zval body_pool_zv;
    array_init(&body_pool_zv);
    size_t total_pool_bytes = 0;

    for (int i = 0; i < BODY_POOL_NUM_CLASSES; i++) {
        zval entry;
        array_init(&entry);
        add_assoc_long(&entry, "slot_bytes", (zend_long)pstats[i].slot_bytes);
        add_assoc_long(&entry, "count",      (zend_long)pstats[i].count);
        add_assoc_long(&entry, "bytes",      (zend_long)pstats[i].bytes);
        add_next_index_zval(&body_pool_zv, &entry);
        total_pool_bytes += pstats[i].bytes;
    }

    add_assoc_zval(return_value, "body_pool", &body_pool_zv);
    add_assoc_long(return_value, "body_pool_total_bytes",
                   (zend_long)total_pool_bytes);
}
/* }}} */

/* Object handlers */

static zend_object *http_server_create(zend_class_entry *ce)
{
    /* PHP wrapper — holds zend_object handle + ref to C-state.
     * std must remain the LAST field of http_server_php so Zend can
     * lay properties out after it (zend_object_alloc semantics). */
    struct http_server_php *php =
        zend_object_alloc(sizeof(struct http_server_php), ce);

    /* C-state — pemalloc'd because it can outlive the PHP wrapper:
     * a live conn that fires a late libuv callback after wrapper
     * free still needs valid memory (its own ref keeps the C-state
     * alive). Last release in http_server_release frees it. */
    http_server_object *server = pecalloc(1, sizeof(*server), /*persistent*/ 0);
    server->refcount = 1;   /* held by the wrapper */

    ZVAL_UNDEF(&server->config);
    http_protocol_handlers_init(&server->protocol_handlers);
    http_log_state_init(&server->log_state);
    conn_arena_init(&server->conn_arena);

    /* Counters live in the embedded slice until a pool worker claims a slab
     * slot (A2); stats_slot < 0 means "no slot owned". */
    server->counters_live = &server->counters;
    server->stats_slot    = -1;
    /* All other fields are pecalloc-zeroed: running=false, stopping=false,
     * server_scope/scope_object/wait_event=NULL, counters/view zeroed,
     * listeners[] zeroed, transit_handlers=NULL, etc. */

    php->server = server;
    zend_object_std_init(&php->std, ce);
    object_properties_init(&php->std, ce);
    php->std.handlers = &http_server_handlers;

    return &php->std;
}

/* get_gc: expose registered handler closures so the cycle collector can
 * break cycles like `closure use ($server) → server->protocol_handlers →
 * closure`. Without this the HttpServer object is unreachable but not
 * freeable, leaking the closure + everything it captures. */
static HashTable *http_server_get_gc(zend_object *obj, zval **table, int *n)
{
    http_server_object *server = http_server_from_obj(obj);
    zend_get_gc_buffer *gc_buffer = zend_get_gc_buffer_create();

    zval *entry;
    ZEND_HASH_FOREACH_VAL(&server->protocol_handlers, entry) {
        if (Z_TYPE_P(entry) == IS_PTR) {
            const zend_fcall_t *fcall = (zend_fcall_t *)Z_PTR_P(entry);
            zend_get_gc_buffer_add_zval(gc_buffer, (zval *)&fcall->fci.function_name);
        }
    } ZEND_HASH_FOREACH_END();

    zend_get_gc_buffer_use(gc_buffer, table, n);
    return zend_std_get_properties(obj);
}

/* Forward decls for the static-handler transit side-car (definition is
 * in the transfer_obj section below — it's logically grouped with
 * TRANSFER/LOAD but http_server_free needs to release it on destruction
 * paths that never went through the pool). */
typedef struct http_server_transit_static http_server_transit_static_t;
static void http_server_transit_static_release(http_server_transit_static_t *t);

static void http_server_free(zend_object *obj)
{
    struct http_server_php *php = http_server_php_from_obj(obj);
    http_server_object *server = php->server;

    /* The wrapper is going away. Live conns still hold refs on the
     * C-state; we no longer need to NULL their back-pointers because
     * the C-state outlives this call until the last conn releases.
     * The libuv shutdown drain that fires post-wrapper-free now sees
     * a valid (still-refcounted) C-state. The C-state's own teardown
     * (listeners, scope, config, TLS ctx) still happens HERE — that's
     * the user-facing "destroy the server" semantic and shouldn't
     * wait on conn lifetimes. */

    /* Stop server if running */
    if (server->running) {
        server->stopping = true;

        /* Stop the deadline watchdog before tearing down listeners —
         * a tick firing during shutdown could try to force-close a
         * conn whose io has already been disposed. */
        http_server_deadline_tick_stop(server);

        for (size_t i = 0; i < server->listener_count; i++) {
            http_server_listener_release(&server->listeners[i]);
        }

#ifdef HAVE_HTTP_SERVER_HTTP3
        for (size_t i = 0; i < server->http3_listener_count; i++) {
            if (server->http3_listeners[i]) {
                http3_listener_destroy(server->http3_listeners[i]);
                server->http3_listeners[i] = NULL;
            }
        }

        server->http3_listener_count = 0;
#endif

        server->running = false;
    }

    /* Defensive on the path where the user dropped the server without
     * calling stop(); idempotent otherwise. */
    http_log_server_stop(&server->log_state);

#ifdef HAVE_HTTP_SERVER_HTTP3
    /* alt_svc_header_value is allocated in start() and normally released
     * in stop(). The release used to live inside the `if (running)`
     * branch above, which leaks it when the user calls $server->stop()
     * explicitly: stop() flips running=false, then free_obj sees running
     * already cleared and skips the cleanup. Move it out so the release
     * fires once on whichever path tears the object down — release(NULL)
     * is a documented no-op so double-free isn't possible. */
    if (server->alt_svc_header_value != NULL) {
        zend_string_release(server->alt_svc_header_value);
        server->alt_svc_header_value = NULL;
    }
#endif

    /* Force-close any conn still on the alive list. With multishot
     * armed for the connection lifetime, an idle conn (no handler in
     * flight, peer hasn't FIN'd yet) holds io_t + the outstanding
     * read req — which in turn keep a ref on the C-state. Without an
     * explicit sweep here those refs never drop and the whole
     * conn/io/parser/strategy chain leaks at script shutdown. Conns
     * with a handler mid-flight set destroy_pending and finalise when
     * the scope release below cancels the handler coroutine. */
    {
        http_connection_t *c = server->conn_arena.alive_head;
        while (c != NULL) {
            http_connection_t *next = c->next_conn;
            http_connection_destroy(c);
            c = next;
        }
    }

    /* Fallback drain (issue #74): if start()'s drain never ran (freed mid-flight,
     * or stop() never called) and we can still suspend, empty the scope here.
     * No-op in teardown/scheduler context — the OBJ_RELEASE below cancels the rest. */
    http_server_drain_scope(server);

    /* Release the scope_object we took at scope creation in start().
     *
     * scope_destroy (the object dtor) will clear scope->scope_object = NULL
     * and call try_to_dispose. Now scope_object == NULL, scope_can_be_disposed
     * stops blocking, and scope_dispose runs cleanly — cancelling any
     * still-alive handler coroutines and freeing the struct. */
    server->server_scope = NULL;

    if (server->scope_object) {
        zend_object *scope_object = server->scope_object;
        server->scope_object = NULL;
        OBJ_RELEASE(scope_object);
    }

#ifdef HAVE_OPENSSL
    /* Tear down shared TLS context after all listeners (and therefore
     * any per-connection sessions that hold SSL* referencing this ctx)
     * have been disposed above. */
    if (server->tls_ctx != NULL) {
        tls_context_free(server->tls_ctx);
        server->tls_ctx = NULL;
    }
#endif

    zval_ptr_dtor(&server->config);

    /* Reactor pool. Normally torn down in the start_pool cleanup path; this is
     * the defensive catch-all if the server is freed without a clean pool
     * exit. */
    http_server_reactor_pool_down(server);

    /* This worker clone's request inbox. Producers (reactors) have quiesced by
     * the time a worker is freed. */
    if (server->worker_inbox != NULL) {
        worker_inbox_free(server->worker_inbox);
        server->worker_inbox = NULL;
    }

    /* Release this worker's stats slab slot (no-op for a standalone/parent
     * server, or if the pool parent already freed the slab). */
    http_server_stats_down(server);

    /* Pool-mode worker ctx array (issue #11). NULL outside pool mode;
     * non-NULL only for parent servers that ran with workers > 1.
     * Each entry's persistent zval shell is released here. */
    if (server->pool_worker_ctx != NULL) {
        pool_worker_ctx_t *ctxs = server->pool_worker_ctx;
        for (int i = 0; i < server->pool_worker_ctx_count; i++) {
            http_server_release_worker_shell(&ctxs[i].server_transit);
        }

        pefree(ctxs, 1);
        server->pool_worker_ctx = NULL;
        server->pool_worker_ctx_count = 0;
    }

    /* Destroy protocol handlers */
    http_protocol_handlers_destroy(&server->protocol_handlers);

    /* Release static handler refs (issue #13). Each entry in
     * static_handler_mounts is a refcounted persistent shared snapshot;
     * we hold one ref per slot. static_handler_objects holds the
     * userland StaticHandler (one per slot, NULL on workers since they
     * have no PHP-side handle). */
    if (server->static_handler_mounts != NULL) {
        for (size_t i = 0; i < server->static_handler_count; i++) {
            if (server->static_handler_mounts[i] != NULL) {
                http_static_handler_shared_release(server->static_handler_mounts[i]);
            }

            if (server->static_handler_objects != NULL
                && server->static_handler_objects[i] != NULL) {
                OBJ_RELEASE(server->static_handler_objects[i]);
            }
        }

        efree(server->static_handler_mounts);
        server->static_handler_mounts = NULL;
    }

    if (server->static_handler_objects != NULL) {
        efree(server->static_handler_objects);
        server->static_handler_objects = NULL;
    }

    if (server->transit_static_mounts != NULL) {
        http_server_transit_static_release(
            (http_server_transit_static_t *) server->transit_static_mounts);
        server->transit_static_mounts = NULL;
    }

    server->static_handler_count    = 0;
    server->static_handler_capacity = 0;

    /* Open file cache — persistent allocations, must be freed
     * before the request-context arenas drop. */
    if (server->static_cache != NULL) {
        http_static_cache_destroy(server->static_cache);
        server->static_cache = NULL;
    }

    /* Destroy the std object — the PHP-side resources die with the
     * wrapper. The C-state stays alive until the last conn releases. */
    zend_object_std_dtor(&php->std);

    /* Drop the wrapper's ref on the C-state. If no live conns remain
     * the C-state finalizes and is freed right here; otherwise the
     * last conn release will finalize. */
    http_server_release(server);
}

/* Final cleanup of the C-state, called from http_server_release when
 * the refcount reaches zero. Anything that depends on C-state-only
 * memory (counters, conn arena, view) is freed here. PHP-facing
 * resources (config zval, std) were already torn down by
 * http_server_free; by the time this runs they were dropped long
 * ago. */
static void http_server_state_finalize(http_server_object *server)
{
    /* Last ref dropped — every live conn has returned its slot to
     * the arena, which means alive_head is empty and the slab can be
     * released. C-state memory itself is freed by the caller (pefree). */
    conn_arena_cleanup(&server->conn_arena);
}

/* ==========================================================================
 * Cross-thread transfer
 *
 * Splitting HttpServer state into two categories makes transfer tractable:
 *   - Blueprint: config zval + protocol-handler closures. Immutable after
 *     __construct locks the config, safely moved to another PHP thread.
 *   - Runtime: server_scope, wait_event, listen_events, counters, flags.
 *     Strictly per-thread — each worker builds its own by calling start().
 *
 * Because listeners are bound with SO_REUSEPORT, each worker thread opens its
 * own socket on the same host:port and the kernel load-balances accept()s.
 * The blueprint carries no sockets; the runtime does, and the runtime is
 * recreated in each thread.
 *
 * Transfer is only valid before start(): once listen_events are live the
 * shell is no longer a pure blueprint. Rejecting the call early keeps the
 * invariant simple.
 * ========================================================================== */

typedef struct {
    http_protocol_type_t protocol;
    zval                 closure;  /* IS_OBJECT, persistent-transferred Closure */
} http_server_transit_handler_t;

/* Four protocol slots matches http_protocol_type_t (excluding UNKNOWN). */
#define HTTP_SERVER_TRANSIT_MAX 4

typedef struct {
    http_server_transit_handler_t entries[HTTP_SERVER_TRANSIT_MAX];
    size_t                        count;
} http_server_transit_handlers_t;

/* Persistent side-car for static-handler fan-out across worker threads
 * (issue #13). Each entry is a pointer into a refcounted persistent
 * snapshot built at addStaticHandler-time; TRANSFER addrefs once per
 * shell, LOAD addrefs once per worker. */
struct http_server_transit_static {
    http_static_handler_t **mounts;   /* pemalloc array */
    size_t                  count;
};

static void http_server_transit_static_release(http_server_transit_static_t *t)
{
    if (t == NULL) return;

    if (t->mounts != NULL) {
        for (size_t i = 0; i < t->count; i++) {
            if (t->mounts[i] != NULL) {
                http_static_handler_shared_release(t->mounts[i]);
            }
        }

        pefree(t->mounts, 1);
    }

    pefree(t, 1);
}

/* Release one worker transit shell INCLUDING the pemalloc'd C-state that
 * transfer_obj(TRANSFER) hung off the wrapper — the engine's generic release
 * frees only the wrapper graph, so the C-state and its side-cars leaked one
 * set per shell (#93 found it: ~10KB per reload rotation). Safe both before
 * and after LOAD (consumed closures release as empty skeletons). */
static void http_server_release_worker_shell(zval *transit)
{
    if (Z_TYPE_P(transit) == IS_OBJECT) {
        http_server_object *shell = http_server_from_obj(Z_OBJ_P(transit));

        if (Z_TYPE(shell->config) == IS_OBJECT) {
            ZEND_ASYNC_THREAD_RELEASE_TRANSFERRED_ZVAL(&shell->config);
        }

        http_server_transit_handlers_t *th = shell->transit_handlers;

        if (th != NULL) {
            for (size_t i = 0; i < th->count; i++) {
                ZEND_ASYNC_THREAD_RELEASE_TRANSFERRED_ZVAL(&th->entries[i].closure);
            }

            pefree(th, 1);
        }

        http_server_transit_static_release(
            (http_server_transit_static_t *) shell->transit_static_mounts);

        pefree(shell, 1);
    }

    ZEND_ASYNC_THREAD_RELEASE_TRANSFERRED_ZVAL(transit);
}

static zend_object *http_server_transfer_obj(
    zend_object *object,
    zend_async_thread_transfer_ctx_t *ctx,
    zend_object_transfer_kind_t kind,
    zend_object_transfer_default_fn default_fn)
{
    if (kind == ZEND_OBJECT_TRANSFER) {
        http_server_object *src = http_server_from_obj(object);

        /* The pool parent is exempt: it owns no live listeners — `running` only
         * means it awaits its workers — and reload() re-transfers fresh shells
         * for every replacement cohort (transit closures are single-load). */
        if (src->running && !src->in_pool_mode) {
            ctx->error = "Cannot transfer HttpServer while running; "
                         "transfer the server before any thread calls start()";
            return NULL;
        }

        /* default_fn pemallocs a raw shell sized for our wrapper. We
         * then pemalloc a fresh C-state and link it into the wrapper.
         * Post-split: the wrapper holds only `server*` + `std`, the
         * full server fields live in the separately-allocated core. */
        zend_object *dst = default_fn(object, ctx, sizeof(struct http_server_php));

        if (UNEXPECTED(dst == NULL)) {
            return NULL;
        }

        struct http_server_php *dst_php = http_server_php_from_obj(dst);
        http_server_object *dst_shell = pecalloc(1, sizeof(*dst_shell), /*persistent*/ 1);
        dst_shell->refcount = 1;
        dst_php->server = dst_shell;

        /* Recursive zval transfer dispatches to HttpServerConfig's transfer_obj,
         * which just addrefs the frozen shared snapshot — cheap. */
        if (Z_TYPE(src->config) == IS_OBJECT) {
            ZEND_ASYNC_THREAD_TRANSFER_ZVAL(ctx, &dst_shell->config, &src->config);

            if (UNEXPECTED(ctx->error)) {
                return NULL;
            }
        }

        http_server_transit_handlers_t *transit =
            pecalloc(1, sizeof(*transit), 1);

        zend_string *key;
        zval *entry;
        ZEND_HASH_MAP_FOREACH_STR_KEY_VAL(&src->protocol_handlers, key, entry) {
            if (Z_TYPE_P(entry) != IS_PTR) {
                continue;
            }

            if (transit->count >= HTTP_SERVER_TRANSIT_MAX) {
                break;
            }

            const zend_fcall_t *fcall = (const zend_fcall_t *) Z_PTR_P(entry);

            zval transferred;
            ZVAL_UNDEF(&transferred);
            ZEND_ASYNC_THREAD_TRANSFER_ZVAL(ctx, &transferred, &fcall->fci.function_name);

            if (UNEXPECTED(ctx->error)) {
                /* Sidecar + partial transfers leak on this path. Rare: only
                 * happens on deep-transfer errors (depth, resource, ref). */
                return NULL;
            }

            transit->entries[transit->count].protocol =
                http_protocol_string_to_type(ZSTR_VAL(key));
            ZVAL_COPY_VALUE(&transit->entries[transit->count].closure, &transferred);
            transit->count++;
        } ZEND_HASH_FOREACH_END();

        dst_shell->transit_handlers = transit;

        /* Static handlers (issue #13). Mounts are persistent +
         * atomic-refcounted shared snapshots: the side-car owns one ref
         * per shell, LOAD addrefs once more into each worker. */
        if (src->static_handler_count > 0) {
            http_server_transit_static_t *st_transit =
                pecalloc(1, sizeof(*st_transit), 1);
            st_transit->mounts = pemalloc(sizeof(http_static_handler_t *)
                                          * src->static_handler_count, 1);
            st_transit->count  = src->static_handler_count;
            for (size_t i = 0; i < src->static_handler_count; i++) {
                st_transit->mounts[i] = src->static_handler_mounts[i];
                http_static_handler_shared_addref(st_transit->mounts[i]);
            }

            dst_shell->transit_static_mounts = st_transit;
        }

        /* Pre-bound AF_UNIX listen fds (workers > 1). POD array copied by
         * value — the fd integers stay valid in the worker thread because
         * all worker threads share one process-wide fd table. */
        memcpy(dst_shell->pool_unix_fds, src->pool_unix_fds,
               sizeof(dst_shell->pool_unix_fds));
        dst_shell->pool_unix_fd_count = src->pool_unix_fd_count;

        /* Same model for TCP on platforms without SO_REUSEPORT load balancing. */
        memcpy(dst_shell->pool_tcp_fds, src->pool_tcp_fds,
               sizeof(dst_shell->pool_tcp_fds));
        dst_shell->pool_tcp_fd_count = src->pool_tcp_fd_count;

        /* Hot-reload beacon (issue #93): plain pointer copy — the pemalloc'd
         * struct is owned by the pool parent and outlives every clone. */
        dst_shell->reload_shared = src->reload_shared;

        return dst;
    }

    /* LOAD */
    http_server_object *src_shell = http_server_from_obj(object);
    /* Pass sizeof(struct http_server_php) so default_fn allocates a
     * properly-sized wrapper. The destination thread's create_object
     * pathway (called by default_fn under LOAD) wires up its own core
     * via emalloc, so we don't need to allocate one here. */
    zend_object *dst = default_fn(object, ctx, 0);

    if (UNEXPECTED(dst == NULL)) {
        return NULL;
    }

    http_server_object *dst_obj = http_server_from_obj(dst);
    /* Worker-clone marker (issue #11). The C-state was just constructed
     * by the destination thread's create_object — when start() runs on
     * this object it must skip the pool-spawn branch and go straight to
     * the standalone event loop, otherwise we'd recursively spawn a
     * fresh ThreadPool on every worker. */
    dst_obj->is_worker_clone = true;

    /* Hot-reload beacon (issue #93): the clone watches epoch from its deadline
     * tick. Snapshot the current value — only a bump AFTER this load retires
     * the worker. */
    dst_obj->reload_shared = src_shell->reload_shared;

    if (dst_obj->reload_shared != NULL) {
        dst_obj->reload_epoch_seen = zend_atomic_int_load(
            &((http_server_reload_shared_t *) dst_obj->reload_shared)->epoch);
    }

    if (Z_TYPE(src_shell->config) == IS_OBJECT) {
        /* create_object zeroed dst->config; drop the UNDEF and install loaded. */
        zval loaded_config;
        ZEND_ASYNC_THREAD_LOAD_ZVAL(ctx, &loaded_config, &src_shell->config);
        zval_ptr_dtor(&dst_obj->config);
        ZVAL_COPY_VALUE(&dst_obj->config, &loaded_config);
    }

    http_server_transit_handlers_t *transit =
        (http_server_transit_handlers_t *) src_shell->transit_handlers;

    if (transit) {
        for (size_t i = 0; i < transit->count; i++) {
            zval closure_zv;
            ZEND_ASYNC_THREAD_LOAD_ZVAL(ctx, &closure_zv, &transit->entries[i].closure);

            if (Z_TYPE(closure_zv) != IS_OBJECT) {
                zval_ptr_dtor(&closure_zv);
                continue;
            }

            zend_fcall_t *fcall = ecalloc(1, sizeof(*fcall));
            fcall->fci.size = sizeof(fcall->fci);
            ZVAL_COPY_VALUE(&fcall->fci.function_name, &closure_zv);

            /* zend_is_callable_ex populates fci_cache from the closure so
             * later http_protocol_get_handler + zend_call_function works. */
            char *error_str = NULL;

            if (!zend_is_callable_ex(&fcall->fci.function_name, NULL, 0, NULL,
                                     &fcall->fci_cache, &error_str)) {
                if (error_str) {
                    efree(error_str);
                }

                zval_ptr_dtor(&fcall->fci.function_name);
                efree(fcall);
                continue;
            }

            if (error_str) {
                efree(error_str);
            }

            /* add_handler addrefs function_name (one ref = HashTable ownership).
             * We then drop our local ref so the closure is owned solely by the
             * HashTable entry; its handler_entry_dtor releases it on cleanup. */
            http_protocol_add_handler(&dst_obj->protocol_handlers,
                transit->entries[i].protocol, fcall);
            zval_ptr_dtor(&closure_zv);

            /* Mirror the protocol_mask bookkeeping that the addXHandler
             * methods do at registration time. The mask is what
             * detect_and_assign_protocol() consults to decide whether to
             * dispatch a parsed request to user code; without this, the
             * loaded handler sits in the HashTable but plain HTTP/1
             * requests are silently dropped on each worker thread. */
            dst_obj->view.protocol_mask |=
                http_protocol_registration_mask(transit->entries[i].protocol);
        }
    }

    /* Static handlers (issue #13). Worker addrefs each shared mount
     * into its own emalloc'd array; the side-car keeps its own refs
     * until the shell is freed. Workers don't materialise PHP
     * StaticHandler objects — userland on a worker has no path to one. */
    http_server_transit_static_t *st_transit =
        (http_server_transit_static_t *) src_shell->transit_static_mounts;

    if (st_transit != NULL && st_transit->count > 0) {
        const size_t n = st_transit->count;
        dst_obj->static_handler_mounts  = emalloc(sizeof(http_static_handler_t *) * n);
        dst_obj->static_handler_objects = ecalloc(n, sizeof(zend_object *));
        for (size_t i = 0; i < n; i++) {
            dst_obj->static_handler_mounts[i] = st_transit->mounts[i];
            http_static_handler_shared_addref(dst_obj->static_handler_mounts[i]);
        }

        dst_obj->static_handler_count    = n;
        dst_obj->static_handler_capacity = n;
        /* Mirror addStaticHandler's protocol-mask side effect so the
         * dispatcher actually routes H1 traffic on the worker. */
        dst_obj->view.protocol_mask |= HTTP_PROTO_MASK_HTTP1;
    }

    /* Pre-bound AF_UNIX listen fds — see the TRANSFER side above. The worker
     * looks these up in its start() to adopt the shared socket. */
    memcpy(dst_obj->pool_unix_fds, src_shell->pool_unix_fds,
           sizeof(dst_obj->pool_unix_fds));
    dst_obj->pool_unix_fd_count = src_shell->pool_unix_fd_count;

    memcpy(dst_obj->pool_tcp_fds, src_shell->pool_tcp_fds,
           sizeof(dst_obj->pool_tcp_fds));
    dst_obj->pool_tcp_fd_count = src_shell->pool_tcp_fd_count;

    return dst;
}

/* {{{ http_server_class_register */
void http_server_class_register(void)
{
    http_server_ce = register_class_TrueAsync_HttpServer();
    http_server_ce->create_object = http_server_create;

    memcpy(&http_server_handlers, &std_object_handlers, sizeof(zend_object_handlers));
    http_server_handlers.offset = offsetof(struct http_server_php, std);
    http_server_handlers.free_obj = http_server_free;
    http_server_handlers.get_gc = http_server_get_gc;
    http_server_handlers.clone_obj = NULL;
    http_server_handlers.transfer_obj = http_server_transfer_obj;

    /* Expose handlers via the class entry so cross-thread LOAD can resolve
     * transfer_obj by class name. TRANSFER dispatches through src->handlers
     * (still live in the source thread), but LOAD has no live source object
     * — it re-enters through ce->default_object_handlers. */
    http_server_ce->default_object_handlers = &http_server_handlers;
}
/* }}} */
