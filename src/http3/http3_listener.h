#ifndef HTTP3_LISTENER_H
#define HTTP3_LISTENER_H

#include <zend.h>
#include <zend_async_API.h>
#include <stdint.h>

#include "http3_packet.h"
#include "http3/http3_stream_pool.h"

/* HTTP/3 UDP listener.
 *
 * Owns one UDP socket (zend_async_io_t* obtained via ZEND_ASYNC_UDP_BIND)
 * and receives datagrams in multishot mode. Datagrams feed ngtcp2; per-
 * peer connections live as http3_connection_t hung off the listener.
 *
 * A listener is created at HttpServer::start() time, torn down at stop()
 * or HttpServer destruction. Lifetime is bound to the owning server. */
typedef struct _http3_listener_s http3_listener_t;

/* ssl_ctx is the OpenSSL SSL_CTX* shared with the TCP+TLS path (from
 * tls_context_t::ctx). Passed as void* to keep openssl/ssl.h out of this
 * header — H3 listener uses it to build per-connection SSL objects via
 * ngtcp2_crypto_ossl_configure_server_session. Must be non-NULL when
 * --enable-http3 is built — addHttp3Listener flags the listener as TLS,
 * and start() constructs the context before spawning. */
http3_listener_t *http3_listener_spawn(const char *host, int port,
                                       void *ssl_ctx, void *server_obj);

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

#endif /* HTTP3_LISTENER_H */
