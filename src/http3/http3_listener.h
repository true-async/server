/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

#ifndef HTTP3_LISTENER_H
#define HTTP3_LISTENER_H

#include <zend.h>
#include <zend_async_API.h>
#include <stdint.h>
#include "win32_compat.h"  /* ssize_t on Windows */

#include "http3_packet.h"
#include "http3/http3_stream_pool.h"
#include "core/worker_registry.h"   /* worker_registry_t */
#include "core/reactor_pool.h"      /* reactor_pool_t */

/* HTTP/3 UDP listener.
 *
 * Owns one UDP socket (zend_async_io_t* obtained via ZEND_ASYNC_UDP_BIND)
 * and receives datagrams in multishot mode. Datagrams feed ngtcp2; per-
 * peer connections live as http3_connection_t hung off the listener.
 *
 * A listener is created at HttpServer::start() time, torn down at stop()
 * or HttpServer destruction. Lifetime is bound to the owning server. */
typedef struct _http3_listener_s http3_listener_t;
typedef struct _http3_connection_s http3_connection_t;  /* defined in http3_connection.h */

/* Thread-clean context for a reactor-OWNED H3 listener.
 *
 * Everything a transport reactor needs to serve QUIC and route parsed requests
 * to PHP workers, WITHOUT the PHP server object — that lives on the parent
 * thread and its ZMM is UB to touch from a reactor. Request-service stats
 * (counters / view / telemetry) are deliberately absent: in the split they are
 * the worker's job (the handler runs there). Built on the parent before spawn,
 * owned by the parent, outlives the listener.
 *
 * When a listener carries one of these it is in "reactor mode": the dispatch
 * path builds a persistent http_request_t and hands it to a worker instead of
 * spawning a handler coroutine locally. A NULL reactor context (the default) is
 * the unchanged single-thread / unit-test path. */
typedef struct {
    worker_registry_t *registry;            /* pick a worker to hand requests to */
    reactor_pool_t    *pool;                 /* reverse path posts back here */
    int                reactor_id;           /* this listener's reactor slot */
    int                n_reactors;            /* reactor count — strided worker ownership */
    uint32_t           socket_buffer_bytes;  /* config scalars resolved on the parent */
    uint32_t           peer_budget;
    uint32_t           max_conns;

    /* Static mounts served on the reactor itself (no PHP, no worker round-trip).
     * Borrowed array of const http_static_handler_t* from the owning server —
     * the mounts outlive the reactor pool (released only at http_server_free,
     * after reactor teardown), so no extra ref is taken. void* to keep the
     * static header out of this widely-included file; cast at the call site. */
    const void        *static_mounts;
    size_t             static_mount_count;

    /* Per-reactor open-file cache for the static path. The cache is persistent
     * (malloc) so it is created/freed on the parent, but NOT thread-safe — each
     * reactor gets its own instance, touched only by its one thread (no locking).
     * NULL when no mount opted into StaticHandler::setOpenFileCache. */
    struct http_static_cache_s *static_cache;
} http3_reactor_ctx_t;

/* ssl_ctx is the OpenSSL SSL_CTX* shared with the TCP+TLS path (from
 * tls_context_t::ctx). Passed as void* to keep openssl/ssl.h out of this
 * header — H3 listener uses it to build per-connection SSL objects via
 * ngtcp2_crypto_ossl_configure_server_session. Must be non-NULL when
 * --enable-http3 is built — addHttp3Listener flags the listener as TLS,
 * and start() constructs the context before spawning. */
/* `reactor_ctx` puts the listener in reactor mode (see http3_reactor_ctx_t):
 * non-NULL means server_obj is the parent's (NULL on the reactor) and request
 * dispatch routes to a worker. NULL is the unchanged single-thread path where
 * server_obj drives local dispatch + config. The listener does NOT own the
 * context (the parent does); it must outlive the listener. */
http3_listener_t *http3_listener_spawn(const char *host, int port,
                                       void *ssl_ctx, void *server_obj,
                                       const http3_reactor_ctx_t *reactor_ctx);

/* The reactor context this listener carries, or NULL in single-thread mode.
 * The H3 dispatch path checks this to decide local-dispatch vs route-to-worker. */
const http3_reactor_ctx_t *http3_listener_reactor_ctx(const http3_listener_t *listener);

/* This listener's reactor id, or -1 in single-thread mode. */
int http3_listener_reactor_id(const http3_listener_t *listener);

/* CID steering group: the set of one endpoint's per-reactor
 * listeners, indexed by reactor id and shared by all of them, so any reactor
 * that receives a stray datagram can forward it to the owner. Opaque; created
 * and freed on the parent. Slots are published atomically as listeners spawn
 * and cleared on teardown. */
typedef struct http3_steer_group_s http3_steer_group_t;
http3_steer_group_t *http3_steer_group_create(reactor_pool_t *pool, int count);
void http3_steer_group_publish(http3_steer_group_t *group, int reactor_id,
                               http3_listener_t *listener);
void http3_steer_group_free(http3_steer_group_t *group);

/* Put a listener into steering mode against its endpoint's group. */
void http3_listener_set_steer(http3_listener_t *listener, http3_steer_group_t *group);

/* If `listener` steers and `data` is a short-header datagram whose DCID decodes
 * to a DIFFERENT reactor (a migrated client rehashed onto us by SO_REUSEPORT),
 * copy it onto the owner reactor's mailbox and return true — the caller must
 * NOT handle it locally. Returns false to let the caller process it normally
 * (it is ours, an Initial, or undecodable). */
bool http3_listener_try_steer(http3_listener_t *listener,
                              uint32_t version,
                              const uint8_t *dcid, size_t dcidlen,
                              const uint8_t *data, size_t datalen, uint8_t ecn,
                              const struct sockaddr *peer, socklen_t peer_len);

/* Reverse-path apply, run ON THE REACTOR thread via
 * reactor_pool_post_exec: `arg` is a response_wire_t* (ownership transfers) that
 * the worker rendered; encode + submit + drain it on the addressed stream, or
 * drop it if the stream is already gone. Declared here (void* arg) so the
 * worker-side sink can post it without pulling in the H3 internals. */
void http3_reactor_apply_response(void *arg);

/* Returns the http_server_object* stashed at spawn time (cast to void*
 * to keep this header free of the public server header). The H3
 * dispatch path uses it to reach `protocol_handlers` and `server_scope`. */
void *http3_listener_server_obj(const http3_listener_t *listener);

/* Snapshot of cumulative counters. Not thread-safe vs the reactor callback
 * but a single worker's reactor + handler coroutines share one OS thread,
 * so a PHP-side read from within the server coroutine is safe. */
typedef struct _http3_listener_stats_s {
    uint64_t datagrams_received;
    uint64_t bytes_received;
    uint64_t datagrams_errored;      /* terminal recv errors observed */
    size_t   last_datagram_size;
    char     last_peer[64];          /* "ip:port" for the most recent datagram, "" if none */

    /* Per-packet QUIC wire classification + handshake/close counters. */
    http3_packet_stats_t packet;
} http3_listener_stats_t;

void http3_listener_get_stats(const http3_listener_t *listener,
                              http3_listener_stats_t *out);

const char *http3_listener_host(const http3_listener_t *listener);
int http3_listener_port(const http3_listener_t *listener);

/* Actual bound UDP port via getsockname — resolves the kernel-assigned port
 * when the listener was spawned with port 0 (http3_listener_port returns the
 * requested 0). Falls back to the requested port on non-Linux / lookup
 * failure. getsockname is thread-agnostic, but callers should still invoke
 * this on the reactor that owns the listener. */
int http3_listener_local_port(const http3_listener_t *listener);

/* Tear down: stop recv, close the IO handle, free the struct. The
 * underlying zend_async_io_t* teardown is non-blocking (libuv close
 * completes on the next reactor tick). Safe to call once per listener. */
void http3_listener_destroy(http3_listener_t *listener);

struct sockaddr;
/* Synchronous best-effort UDP send. On Linux (raw-fd path) issues one
 * sendmsg(MSG_DONTWAIT). On other platforms (incl. Windows) falls back
 * to the libuv udp_try_send / udp_sendto pair (the legacy code path);
 * ECN and GSO are silently dropped there.
 *
 * Returns the number of bytes accepted by the kernel (== len on success
 * because UDP is all-or-nothing), or a negative errno on failure (-EAGAIN
 * when the kernel buffer is full). */
ssize_t http3_listener_send_packet(http3_listener_t *listener,
                                   const void *buf, size_t len, uint8_t ecn,
                                   const struct sockaddr *peer,
                                   socklen_t peer_len);

/* GSO send: one sendmsg with cmsg(UDP_SEGMENT, segsize). The buffer
 * `buf` of length `total_len` is interpreted by the kernel as N
 * segments of `segsize` bytes (the last segment can be shorter, all
 * earlier ones MUST equal segsize). Linux 4.18+; on platforms without
 * UDP_SEGMENT (Windows, macOS) this falls back to a per-segment
 * send_packet loop — correctness preserved at a throughput cost.
 *
 * `ecn` is the IP_TOS / IPV6_TCLASS byte to echo on the outbound
 * datagrams (typically the value ngtcp2 returned in pi.ecn). 0 means
 * no ECN cmsg.
 *
 * If the kernel refuses GSO (EIO from the wrong NIC, EINVAL from
 * mis-sized buffer, etc.), the helper falls back to per-segment
 * sendmsg loop so the data still goes out — at the cost of throughput,
 * not correctness. */
ssize_t http3_listener_send_gso(http3_listener_t *listener,
                                const void *buf, size_t total_len,
                                size_t segsize, uint8_t ecn,
                                const struct sockaddr *peer,
                                socklen_t peer_len);

/* Per-listener slab pool for http3_stream_t. All conns under this
 * listener share the same pool — H3 listener is single-thread per
 * worker, so no locking. Returns non-NULL once the listener is up. */
http3_stream_pool_t *http3_listener_stream_pool(http3_listener_t *listener);

/* Deferred output. mark_flush records that `conn` produced
 * ngtcp2 output this tick (idempotent — guarded by conn->in_dirty);
 * flush_dirty drains every marked conn once and clears the list. The
 * read path marks instead of draining, so a multi-datagram burst to one
 * conn collapses to a single drain (one GSO sendmsg) per tick. */
void http3_listener_mark_flush(http3_listener_t *listener, http3_connection_t *conn);
void http3_listener_flush_dirty(http3_listener_t *listener);

/* Reactor drain-batch epilogue: flush every listener that took a forwarded
 * (steered) datagram this batch exactly once. Register with
 * reactor_pool_set_drain_epilogue before reactors start. */
void http3_reactor_steer_flush_epilogue(void);

#endif /* HTTP3_LISTENER_H */
