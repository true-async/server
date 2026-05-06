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
#include "php_http_server.h"
#include "core/http_connection.h"
#include "core/http_connection_internal.h"
#include "core/conn_arena.h"
#include "core/http_protocol_handlers.h"
#include "core/http_protocol_strategy.h"
#include "core/tls_layer.h"
#include "log/http_log.h"
#ifdef HAVE_HTTP_SERVER_HTTP3
# include "http3/http3_listener.h"
#endif

/* Backpressure tunables. Hard-cap hysteresis ratio: pause_low = ratio *
 * pause_high. CoDel interval is the RFC 8289 constant — not tunable.
 * CoDel target is read from HttpServerConfig::backpressure_target_ms at
 * start() and can be overridden via env CODEL_TARGET_MS. */
#define BACKPRESSURE_PAUSE_LOW_RATIO   80   /* percent */
#define CODEL_INTERVAL_NS              (100ULL * 1000000ULL)  /* 100 ms */

/* php_network.h supplies sys/socket.h / winsock. No POSIX-only headers
 * remain — the listen socket is owned by the reactor, and closesocket /
 * SOCK_ERR / php_socket_errno are cross-platform shims. */
#include <stdint.h>

/* See http_connection.c for rationale. */
#ifndef MSG_NOSIGNAL
# define MSG_NOSIGNAL 0
#endif

/* Include generated arginfo */
#include "../stubs/HttpServer.php_arginfo.h"

/* Max listeners */
#define MAX_LISTENERS 16

/* Listener info. The listen_event owns the socket fd and the libuv handle;
 * we just store the handle plus any per-listener flags. protocol_mask is
 * the per-listener HTTP_PROTO_MASK_* set; carried onto each spawned
 * http_connection_t and consulted by detect_and_assign_protocol. */
typedef struct {
    zend_async_listen_event_t   *listen_event;
    bool                         tls;
    uint32_t                     protocol_mask;
} http_listener_t;

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

#ifdef HAVE_HTTP_SERVER_HTTP3
    /* HTTP/3 UDP listeners — parallel to TCP listeners[] because they have
     * different transport semantics (no accept(), no per-connection fd) and
     * are teardown-driven by http3_listener_destroy(). */
    http3_listener_t        *http3_listeners[MAX_LISTENERS];
    size_t                   http3_listener_count;

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
     * check. Connections cache &server->counters at create time. */
    http_server_counters_t   counters;

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

    /* Transit sidecar — non-NULL only in the persistent shell created by
     * transfer_obj(TRANSFER). Holds pemalloc-copied closures so the LOAD
     * side can rebuild fcall_t entries in the destination thread's heap.
     * Always NULL in the source thread's emalloc object. */
    void                    *transit_handlers;
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
    return (struct http_server_php *)((char *)(obj) - XtOffsetOf(struct http_server_php, std));
}

static inline http_server_object *http_server_from_obj(zend_object *obj) {
    return http_server_php_from_obj(obj)->server;
}
#define Z_HTTP_SERVER_P(zv) http_server_from_obj(Z_OBJ_P(zv))

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
        return 60000;
    }
    uint32_t tick = m / 2;
    if (tick < 250) tick = 250;
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
        && server->counters.active_requests >= server->max_inflight_requests) {
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
zend_string *http_server_get_alt_svc_value(const http_server_object *const server)
{
#ifdef HAVE_HTTP_SERVER_HTTP3
    return server != NULL ? server->alt_svc_header_value : NULL;
#else
    (void)server;
    return NULL;
#endif
}

uint64_t http_server_get_max_connection_age_ns(const http_server_object *const server)
{
    return server != NULL ? server->max_connection_age_ns : 0;
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
    .http3_alt_svc_enabled = true,
};

/* Counters / view accessors. Always return a non-NULL pointer; callers
 * cache once at create time. */
http_server_counters_t *http_server_counters(http_server_object *const server)
{
    return server != NULL ? &server->counters : &http_server_counters_dummy;
}

const http_server_view_t *http_server_view(const http_server_object *const server)
{
    return server != NULL ? &server->view : &http_server_view_default;
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

void http_server_trigger_drain(http_server_object *const server)
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
http_server_drain_eval_t http_server_drain_evaluate(http_server_object *const server,
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

bool http_server_should_drain_now(http_server_object *const server,
                                  http_connection_t *const conn,
                                  uint64_t now_ns)
{
    if (conn == NULL) {
        return false;
    }
    const http_server_drain_eval_t r = http_server_drain_evaluate(server,
        conn->drain_pending,
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
    real->counters  = &server->counters;
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
     * callback). Enable both bits so dual-protocol listeners keep
     * working; h2-only deployments use addHttp2Handler exclusively. */
    server->view.protocol_mask |= HTTP_PROTO_MASK_HTTP1 | HTTP_PROTO_MASK_HTTP2;
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
    server->view.protocol_mask |= HTTP_PROTO_MASK_WS;
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
    server->view.protocol_mask |= HTTP_PROTO_MASK_HTTP2;
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

    /* Store handler - will be used when gRPC support is implemented */
    http_protocol_add_handler_internal(
        INTERNAL_FUNCTION_PARAM_PASSTHRU,
        &server->protocol_handlers,
        HTTP_PROTOCOL_GRPC,
        Z_OBJ_P(ZEND_THIS)
    );
    server->view.protocol_mask |= HTTP_PROTO_MASK_GRPC;
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
        send(client_fd, response, strlen(response), MSG_NOSIGNAL);
        closesocket(client_fd);
        /* Make sure listeners are paused so we stop accepting new ones. */
        if (!server->listeners_paused) {
            /* Race fallback — still counts as overload, drain existing. */
            http_server_pause_listeners(server, /*drain_connections=*/true);
        }
        return;
    }

    /* Pre-accept sanity check: at least one HTTP-family handler must be
     * registered. Actual handler selection happens later, after protocol
     * detection matches the connection to a specific strategy. */
    zend_fcall_t *handler =
        http_protocol_get_handler(&server->protocol_handlers, HTTP_PROTOCOL_HTTP1);
    if (handler == NULL) {
        handler = http_protocol_get_handler(&server->protocol_handlers, HTTP_PROTOCOL_HTTP2);
    }
    if (UNEXPECTED(!handler)) {
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
    zval  server_transit;   /* per-worker persistent shell */
} pool_worker_ctx_t;

typedef struct {
    int                          pending;     /* workers not yet done */
    zend_async_event_t          *all_done;    /* fires when pending == 0 */
    zend_async_event_callback_t  cb;          /* embedded — recovered via XtOffsetOf */
} pool_await_state_t;

static void pool_worker_handler(zend_async_event_t *event, void *const vctx)
{
    (void)event;
    pool_worker_ctx_t *const wctx = (pool_worker_ctx_t *)vctx;

    zval server_zv;
    ZVAL_UNDEF(&server_zv);
    ZEND_ASYNC_THREAD_LOAD_ZVAL_TOPLEVEL(&server_zv, &wctx->server_transit);

    if (EXPECTED(Z_TYPE(server_zv) == IS_OBJECT)) {
        zend_call_method_with_0_params(Z_OBJ(server_zv), NULL, NULL, "start", NULL);
        /* Exception in worker is captured by the future state via the
         * ext/async dispatcher — clear locally so the handler returns
         * cleanly. */
        if (UNEXPECTED(EG(exception))) {
            zend_clear_exception();
        }
    }
    zval_ptr_dtor(&server_zv);
    /* ctx is part of the parent's pool_worker_ctx array; freed once
     * by http_server_free when the parent server destructs. */
}

static void pool_worker_done_cb(zend_async_event_t *event,
                                zend_async_event_callback_t *cb,
                                void *result, zend_object *exception)
{
    (void)event; (void)result; (void)exception;
    /* Callbacks fire on the parent thread (cross-thread wakeup is
     * already serialized by the reactor) — no atomicity needed. */
    pool_await_state_t *const st = (pool_await_state_t *)
        ((char *)cb - XtOffsetOf(pool_await_state_t, cb));
    if (--st->pending == 0 && st->all_done != NULL) {
        ZEND_ASYNC_CALLBACKS_NOTIFY(st->all_done, NULL, NULL);
    }
}

static int http_server_start_pool(http_server_object *const server,
                                  zval *const this_zv,
                                  const int workers)
{
    if (UNEXPECTED(zend_async_new_thread_pool_fn == NULL)) {
        zend_throw_exception(http_server_runtime_exception_ce,
            "ThreadPool API is not registered — load true_async first", 0);
        return FAILURE;
    }

    /* queue_size = workers so submit doesn't block before workers reach
     * the receive loop — fresh-pool boot-up is otherwise faster on the
     * parent than on worker threads. */
    zend_async_thread_pool_t *const pool =
        ZEND_ASYNC_NEW_THREAD_POOL((int32_t)workers, (int32_t)workers);
    if (UNEXPECTED(pool == NULL || pool->submit_internal == NULL)) {
        if (pool != NULL) {
            ZEND_THREAD_POOL_DELREF(pool);
        }
        zend_throw_exception(http_server_runtime_exception_ce,
            "ThreadPool->submit_internal not available — true_async too old", 0);
        return FAILURE;
    }

    /* One persistent shell per worker. Allocate the whole array up
     * front so cleanup is a single pefree in http_server_free. */
    pool_worker_ctx_t *const ctxs = pemalloc(sizeof(*ctxs) * (size_t)workers, 1);
    for (int i = 0; i < workers; i++) {
        ZVAL_UNDEF(&ctxs[i].server_transit);
        ZEND_ASYNC_THREAD_TRANSFER_ZVAL_TOPLEVEL(&ctxs[i].server_transit, this_zv);
        if (UNEXPECTED(EG(exception))) {
            /* Roll back transfers we already did. */
            for (int j = 0; j <= i; j++) {
                ZEND_ASYNC_THREAD_RELEASE_TRANSFERRED_ZVAL(&ctxs[j].server_transit);
            }
            pefree(ctxs, 1);
            ZEND_THREAD_POOL_DELREF(pool);
            return FAILURE;
        }
    }
    server->pool_worker_ctx       = ctxs;
    server->pool_worker_ctx_count = workers;

    pool_await_state_t *const st = ecalloc(1, sizeof(*st));
    st->pending = workers;
    st->all_done = create_server_wait_event();
    st->cb.callback = pool_worker_done_cb;

    int rc = FAILURE;
    for (int i = 0; i < workers; i++) {
        zend_async_event_t *const worker_evt =
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
    }

    server->in_pool_mode = true;
    server->running = true;

    /* Suspend until all submitted workers report done. */
    zend_coroutine_t *const coroutine = ZEND_ASYNC_CURRENT_COROUTINE;
    if (UNEXPECTED(ZEND_ASYNC_WAKER_NEW(coroutine) == NULL)) {
        zend_throw_exception(http_server_runtime_exception_ce,
            "Failed to create waker for pool parent", 0);
        goto cleanup;
    }
    zend_async_resume_when(coroutine, st->all_done, true,
                           zend_async_waker_callback_resolve, NULL);
    ZEND_ASYNC_SUSPEND();
    zend_async_waker_clean(coroutine);
    if (EG(exception)) {
        zend_clear_exception();
    }

    rc = (st->pending == 0) ? SUCCESS : FAILURE;

cleanup:
    server->running = false;
    server->stopping = false;
    server->in_pool_mode = false;
    if (st->all_done != NULL) {
        st->all_done->dispose(st->all_done);
    }
    efree(st);
    ZEND_THREAD_POOL_DELREF(pool);
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

    /* Require at least one handler that serves HTTP requests (h1 or h2).
     * h2-only deployments use addHttp2Handler; dual-protocol deployments
     * use addHttpHandler which covers both. */
    if (!http_protocol_has_handler(&server->protocol_handlers, HTTP_PROTOCOL_HTTP1) &&
        !http_protocol_has_handler(&server->protocol_handlers, HTTP_PROTOCOL_HTTP2)) {
        zend_throw_exception(http_server_runtime_exception_ce,
            "No HTTP handler registered. Call addHttpHandler() or addHttp2Handler() first", 0);
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
    server->counters.active_requests = 0;

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

    zend_call_method_with_0_params(Z_OBJ(server->config), NULL, NULL, "isHttp3AltSvcEnabled", &retval);
    server->view.http3_alt_svc_enabled = (Z_TYPE(retval) == IS_TRUE);

    zend_call_method_with_0_params(Z_OBJ(server->config), NULL, NULL, "isTelemetryEnabled", &retval);
    server->view.telemetry_enabled = (Z_TYPE(retval) == IS_TRUE);

    /* Stamps drive CoDel sojourn samples and the telemetry aggregate
     * (sojourn_sum / service_sum / sojourn_max). Drain falls back to a
     * fresh hrtime when end_ns is 0, so it does not require stamps. */
    server->view.sample_stamps_enabled =
        (server->codel_target_ns != 0) || server->view.telemetry_enabled;

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
    /* Keep our own pointer to the scope's zend_object. scope_destroy (the
     * dtor_obj handler) runs during request shutdown's dtor phase BEFORE
     * our http_server_free (the free_obj handler) runs — and it nulls
     * scope->scope_object, so we can't reach the object via server_scope
     * at that point. Stashing our own ref keeps it reachable until we
     * explicitly release it. */
    server->scope_object = server->server_scope->scope_object;

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
            zval *const tls_flag =
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

            const char *const cert_path =
                (Z_TYPE(cert_zv) == IS_STRING) ? Z_STRVAL(cert_zv) : NULL;
            const char *const key_path =
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
            zval *const tls_flag =
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
            int port = Z_LVAL_P(port_zv);
            bool tls = tls_zv && zend_is_true(tls_zv);
            uint32_t protocol_mask = (mask_zv && Z_TYPE_P(mask_zv) == IS_LONG)
                ? (uint32_t)Z_LVAL_P(mask_zv)
                : (HTTP_PROTO_MASK_HTTP1 | HTTP_PROTO_MASK_HTTP2);

            /* REUSEPORT enables kernel-level load balancing across worker
             * processes / threads sharing host:port. The reactor creates
             * the socket, binds with UV_TCP_REUSEPORT, and sets up uv_listen
             * + per-accept notifications. */
            /* Windows libuv does not implement UV_TCP_REUSEPORT (returns
             * ENOTSUP at bind). Only request it on platforms that support it. */
#ifdef PHP_WIN32
            unsigned int listen_flags = 0;
#else
            unsigned int listen_flags = ZEND_ASYNC_LISTEN_F_REUSEPORT;
#endif
            zend_async_listen_event_t *listen_event = ZEND_ASYNC_SOCKET_LISTEN_EX(
                host, port, server->backlog, listen_flags, 0);

            if (!listen_event) {
                /* Cleanup already-created listen events */
                for (size_t i = 0; i < server->listener_count; i++) {
                    server->listeners[i].listen_event->base.stop(&server->listeners[i].listen_event->base);
                    server->listeners[i].listen_event->base.dispose(&server->listeners[i].listen_event->base);
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
                /* ZEND_ASYNC_SOCKET_LISTEN already threw an async exception with
                 * the underlying uv_strerror; propagate it. */
                RETURN_FALSE;
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
                /* server_obj: */ server);
            if (!h3) {
                /* Unwind both TCP and H3 listeners — start() is all-or-nothing. */
                for (size_t i = 0; i < server->listener_count; i++) {
                    server->listeners[i].listen_event->base.stop(&server->listeners[i].listen_event->base);
                    server->listeners[i].listen_event->base.dispose(&server->listeners[i].listen_event->base);
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
        /* TODO: Handle "unix" type */
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
        zval *log_stream = (Z_TYPE(cfg->log_stream) != IS_UNDEF)
                         ? &cfg->log_stream : NULL;
        http_log_server_start(&server->log_state,
                              (http_log_severity_t)cfg->log_severity,
                              log_stream);
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

    /* Suspend coroutine - control returns to event loop */
    ZEND_ASYNC_SUSPEND();

    /* Cleanup waker after resume */
    zend_async_waker_clean(coroutine);

    /* Clear any cancellation exceptions */
    if (EG(exception)) {
        zend_clear_exception();
    }

    RETURN_TRUE;
}
/* }}} */

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

    /* Stop the deadline watchdog FIRST so the periodic timer no longer
     * keeps the libuv loop alive past server stop. Without this, the
     * loop drains forever and stop() never lets the script exit. */
    http_server_deadline_tick_stop(server);

    /* Stop all listen events. dispose() also closes the underlying uv_tcp_t
     * and frees the fd — no separate closesocket call needed. */
    for (size_t i = 0; i < server->listener_count; i++) {
        if (server->listeners[i].listen_event) {
            server->listeners[i].listen_event->base.stop(&server->listeners[i].listen_event->base);
            server->listeners[i].listen_event->base.dispose(&server->listeners[i].listen_event->base);
            server->listeners[i].listen_event = NULL;
        }
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
    http_logf_info(&server->log_state, "server.stop");
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

    RETURN_TRUE;
}
/* }}} */

/* {{{ proto HttpServer::isRunning(): bool */
ZEND_METHOD(TrueAsync_HttpServer, isRunning)
{
    ZEND_PARSE_PARAMETERS_NONE();

    http_server_object *server = Z_HTTP_SERVER_P(ZEND_THIS);
    RETURN_BOOL(server->running);
}
/* }}} */

/* {{{ proto HttpServer::getTelemetry(): array */
ZEND_METHOD(TrueAsync_HttpServer, getTelemetry)
{
    ZEND_PARSE_PARAMETERS_NONE();

    http_server_object *server = Z_HTTP_SERVER_P(ZEND_THIS);

    array_init(return_value);
    add_assoc_long(return_value, "total_requests", (zend_long)server->counters.total_requests);
    add_assoc_long(return_value, "active_connections", server->active_connections);
    add_assoc_long(return_value, "active_requests", (zend_long)server->counters.active_requests);
    add_assoc_long(return_value, "max_inflight_requests", (zend_long)server->max_inflight_requests);
    add_assoc_long(return_value, "requests_shed_total", (zend_long)server->counters.requests_shed_total);
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
    add_assoc_long  (return_value, "tls_bytes_plaintext_in_total",   (zend_long)server->counters.tls_bytes_plaintext_in_total);
    add_assoc_long  (return_value, "tls_bytes_plaintext_out_total",  (zend_long)server->counters.tls_bytes_plaintext_out_total);
    add_assoc_long  (return_value, "tls_bytes_ciphertext_in_total",  (zend_long)server->counters.tls_bytes_ciphertext_in_total);
    add_assoc_long  (return_value, "tls_bytes_ciphertext_out_total", (zend_long)server->counters.tls_bytes_ciphertext_out_total);
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
                   (zend_long)server->counters.h2_goaway_sent_total);
    add_assoc_long(return_value, "h3_goaway_sent_total",
                   (zend_long)server->counters.h3_goaway_sent_total);
    add_assoc_long(return_value, "h1_connection_close_sent_total",
                   (zend_long)server->counters.h1_connection_close_sent_total);
    add_assoc_long(return_value, "connections_force_closed_total",
                   (zend_long)server->connections_force_closed_total);

    /* Streaming-response telemetry. */
    add_assoc_long(return_value, "streaming_responses_total",
                   (zend_long)server->counters.streaming_responses_total);
    add_assoc_long(return_value, "stream_send_calls_total",
                   (zend_long)server->counters.stream_send_calls_total);
    add_assoc_long(return_value, "stream_send_backpressure_events_total",
                   (zend_long)server->counters.stream_send_backpressure_events_total);
    add_assoc_long(return_value, "stream_bytes_sent_total",
                   (zend_long)server->counters.stream_bytes_sent_total);

    /* HTTP/2 stream-level telemetry. */
    add_assoc_long(return_value, "h2_streams_active",
                   (zend_long)server->counters.h2_streams_active);
    add_assoc_long(return_value, "h2_streams_opened_total",
                   (zend_long)server->counters.h2_streams_opened_total);
    add_assoc_long(return_value, "h2_streams_reset_by_peer_total",
                   (zend_long)server->counters.h2_streams_reset_by_peer_total);
    add_assoc_long(return_value, "h2_streams_refused_total",
                   (zend_long)server->counters.h2_streams_refused_total);
    add_assoc_long(return_value, "h2_goaway_recv_total",
                   (zend_long)server->counters.h2_goaway_recv_total);
    add_assoc_long(return_value, "h2_data_recv_bytes_total",
                   (zend_long)server->counters.h2_data_recv_bytes_total);
    add_assoc_long(return_value, "h2_data_sent_bytes_total",
                   (zend_long)server->counters.h2_data_sent_bytes_total);
    add_assoc_long(return_value, "h2_ping_rtt_ns",
                   (zend_long)server->counters.h2_ping_rtt_ns);
}
/* }}} */

/* {{{ proto HttpServer::resetTelemetry(): bool */
ZEND_METHOD(TrueAsync_HttpServer, resetTelemetry)
{
    ZEND_PARSE_PARAMETERS_NONE();

    http_server_object *server = Z_HTTP_SERVER_P(ZEND_THIS);

    server->counters.total_requests = 0;
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
    server->counters.tls_bytes_plaintext_in_total   = 0;
    server->counters.tls_bytes_plaintext_out_total  = 0;
    server->counters.tls_bytes_ciphertext_in_total  = 0;
    server->counters.tls_bytes_ciphertext_out_total = 0;
    server->tls_ktls_tx_total              = 0;
    server->tls_ktls_rx_total              = 0;
    server->parse_errors_4xx_total         = 0;
    server->parse_errors_400_total         = 0;
    server->parse_errors_413_total         = 0;
    server->parse_errors_414_total         = 0;
    server->parse_errors_431_total         = 0;
    server->parse_errors_503_total         = 0;
    server->counters.requests_shed_total            = 0;
    server->counters.h2_streams_refused_total       = 0;

    /* Drain counters. drain_epoch_current / drain_last_fired_ns are
     * runtime state, NOT cleared (same rationale as paused_since_ns). */
    server->connections_drained_reactive_total   = 0;
    server->connections_drained_proactive_total  = 0;
    server->counters.h2_goaway_sent_total                 = 0;
    server->counters.h3_goaway_sent_total                 = 0;
    server->counters.h1_connection_close_sent_total       = 0;
    server->connections_force_closed_total       = 0;
    server->drain_events_reactive_total          = 0;
    server->drain_events_cooldown_blocked_total  = 0;
    /* Streaming counters. */
    memset(&server->counters, 0, sizeof(server->counters));
    /* HTTP/2 stream telemetry. Active count is
     * live state — NOT cleared (otherwise operators would see a
     * negative drift as close events decrement past zero). */
    server->counters.h2_streams_opened_total                = 0;
    server->counters.h2_streams_reset_by_peer_total         = 0;
    server->counters.h2_goaway_recv_total                   = 0;
    server->counters.h2_data_recv_bytes_total               = 0;
    server->counters.h2_data_sent_bytes_total               = 0;
    server->counters.h2_ping_rtt_ns                         = 0;
    /* Don't reset paused_since_ns or CoDel runtime state — those track
     * live pause and would confuse the control loop if cleared mid-flight. */

    RETURN_TRUE;
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

/* {{{ proto HttpServer::getHttp3Stats(): array
 *
 * Per-listener observability for the HTTP/3 bootstrap path. Returns an
 * array indexed by listener position; each entry has host, port,
 * datagrams_received, bytes_received, datagrams_errored, last_datagram_size,
 * last_peer. Counters let tests confirm the UDP pipe is live end-to-end.
 */
ZEND_METHOD(TrueAsync_HttpServer, getHttp3Stats)
{
    ZEND_PARSE_PARAMETERS_NONE();

    array_init(return_value);

#ifdef HAVE_HTTP_SERVER_HTTP3
    http_server_object *server = Z_HTTP_SERVER_P(ZEND_THIS);

    for (size_t i = 0; i < server->http3_listener_count; i++) {
        http3_listener_t *l = server->http3_listeners[i];
        if (l == NULL) continue;

        http3_listener_stats_t s;
        http3_listener_get_stats(l, &s);

        zval entry;
        array_init(&entry);
        add_assoc_string(&entry, "host", (char *)http3_listener_host(l));
        add_assoc_long  (&entry, "port", http3_listener_port(l));
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
        /* Audit hardening counters. */
        add_assoc_long(&entry, "h3_framing_error",           (zend_long)s.packet.h3_framing_error);
        add_assoc_long(&entry, "quic_drain_iter_cap_hit",    (zend_long)s.packet.quic_drain_iter_cap_hit);

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
            if (server->listeners[i].listen_event) {
                server->listeners[i].listen_event->base.stop(&server->listeners[i].listen_event->base);
                server->listeners[i].listen_event->base.dispose(&server->listeners[i].listen_event->base);
                server->listeners[i].listen_event = NULL;
            }
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

    /* Pool-mode worker ctx array (issue #11). NULL outside pool mode;
     * non-NULL only for parent servers that ran with workers > 1.
     * Each entry's persistent zval shell is released here. */
    if (server->pool_worker_ctx != NULL) {
        pool_worker_ctx_t *const ctxs = server->pool_worker_ctx;
        for (int i = 0; i < server->pool_worker_ctx_count; i++) {
            ZEND_ASYNC_THREAD_RELEASE_TRANSFERRED_ZVAL(&ctxs[i].server_transit);
        }
        pefree(ctxs, 1);
        server->pool_worker_ctx = NULL;
        server->pool_worker_ctx_count = 0;
    }

    /* Destroy protocol handlers */
    http_protocol_handlers_destroy(&server->protocol_handlers);

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

static zend_object *http_server_transfer_obj(
    zend_object *object,
    zend_async_thread_transfer_ctx_t *ctx,
    zend_object_transfer_kind_t kind,
    zend_object_transfer_default_fn default_fn)
{
    if (kind == ZEND_OBJECT_TRANSFER) {
        http_server_object *src = http_server_from_obj(object);

        if (src->running) {
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
            switch (transit->entries[i].protocol) {
                case HTTP_PROTOCOL_HTTP1:
                    dst_obj->view.protocol_mask |=
                        HTTP_PROTO_MASK_HTTP1 | HTTP_PROTO_MASK_HTTP2;
                    break;
                case HTTP_PROTOCOL_HTTP2:
                    dst_obj->view.protocol_mask |= HTTP_PROTO_MASK_HTTP2;
                    break;
                case HTTP_PROTOCOL_WEBSOCKET:
                    dst_obj->view.protocol_mask |= HTTP_PROTO_MASK_WS;
                    break;
                case HTTP_PROTOCOL_GRPC:
                    dst_obj->view.protocol_mask |= HTTP_PROTO_MASK_GRPC;
                    break;
                default:
                    break;
            }
        }
    }

    return dst;
}

/* {{{ http_server_class_register */
void http_server_class_register(void)
{
    http_server_ce = register_class_TrueAsync_HttpServer();
    http_server_ce->create_object = http_server_create;

    memcpy(&http_server_handlers, &std_object_handlers, sizeof(zend_object_handlers));
    http_server_handlers.offset = XtOffsetOf(struct http_server_php, std);
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
