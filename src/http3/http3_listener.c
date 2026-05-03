#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <php.h>
#include "http3_listener.h"
#include "http3_packet.h"
#include "http3_connection.h"
#include "php_http_server.h"
#include "log/http_log.h"


#include <zend_async_API.h>
#include <openssl/rand.h>
#include <openssl/crypto.h>
#include <string.h>
#include <errno.h>

#ifdef PHP_WIN32
# include <winsock2.h>
# include <ws2tcpip.h>
#else
# include <arpa/inet.h>
# include <netinet/in.h>
# include <netinet/udp.h>
# include <netinet/ip.h>
# include <sys/socket.h>
# include <unistd.h>
# include <fcntl.h>
# ifdef __linux__
#  include <linux/errqueue.h>   /* struct sock_extended_err */
# endif
#endif

/* QUIC datagram buffer. 1600 covers the QUIC-recommended 1500-byte MTU plus
 * ngtcp2 headroom. We use stack-allocated arrays of this for recvmmsg(2)
 * batches, mirroring h2o's pattern (lib/http3/common.c:787). */
#define HTTP3_LISTENER_DGRAM_SIZE 1600

/* recvmmsg batch cap. h2o uses 10 — large enough to amortise the syscall,
 * small enough to keep QUIC ACK pacing responsive (one ACK per ≤10 ack-
 * eliciting packets). See h2o lib/http3/common.c:819-822. */
#define HTTP3_LISTENER_RECV_BATCH 10

struct _http3_listener_s {
    /* Linux raw-fd path. fd >= 0 iff this listener is using the
     * recvmmsg/sendmsg fast path; otherwise the legacy udp_io path
     * is in use. The two are mutually exclusive at spawn time. */
    int                          fd;
    zend_async_poll_event_t     *poll_event;
    zend_async_event_callback_t *poll_cb;
    int                          family;    /* AF_INET / AF_INET6 of bind */
    /* Latched off after first sendmsg(UDP_SEGMENT) refusal (EIO/EINVAL).
     * Avoids re-paying the cmsg-build + kernel-reject cost on every
     * subsequent send when the underlying NIC/route doesn't support GSO.
     * One-way switch — not reset until listener teardown. */
    bool                         gso_disabled;
    /* Set by send_packet/send_gso when sendmsg returns an error; cleared
     * by the next poll-cb errqueue drain. Avoids paying the extra
     * recvmsg(MSG_ERRQUEUE) syscall on every wakeup of a healthy
     * socket — most setups never see ICMP errors, so the queue is
     * almost always empty. */
    bool                         errq_pending;

    /* Legacy libuv-wrapped UDP path — non-Linux fallback. Both pointers
     * are NULL on Linux; on other platforms fd stays -1 and these are set. */
    zend_async_io_t             *udp_io;
    zend_async_event_callback_t *recv_cb;
    zend_async_udp_req_t        *recv_req;
    char                      *host;        /* estrdup */
    int                        port;
    bool                       closed;

    /* DCID → http3_connection_t* routing table. Every conn appears under
     * one or two keys (server SCID + client's original DCID) so both
     * retransmitted Initials and post-handshake short-header packets
     * route correctly. Non-owning: teardown does not iterate this table,
     * it walks the intrusive `conn_list` below instead. */
    HashTable                 *conn_map;

    /* Intrusive ownership list of http3_connection_t*. `conn_list` head,
     * conn->next links. Teardown walks this chain once, calling
     * http3_connection_free on each — no risk of visiting the same conn
     * twice as we would if we iterated conn_map. */
    http3_connection_t        *conn_list;

    /* SSL_CTX* shared with the TCP/TLS path. Non-owning — the server
     * constructs and tears it down. Stored as void* to keep openssl.h
     * out of the public listener header. */
    void                      *ssl_ctx;

    /* Back-pointer to the http_server_object that owns
     * us. Stored as void* to keep php_http_server.h out of public
     * headers; H3 dispatch reaches handler fcall + server_scope by
     * casting and calling http_protocol_get_handler. NULL when the
     * listener is driven directly by a unit test. */
    void                      *server_obj;

    /* 32-byte HMAC-SHA256 key used to derive stateless-reset
     * tokens (RFC 9000 §10.3). Generated once at spawn from OpenSSL's
     * DRBG; per-process lifetime, so a server restart invalidates all
     * outstanding tokens (acceptable trade-off vs. on-disk storage).
     * Same key is used by get_new_connection_id_cb so the tokens we
     * issue match what we send on a stateless reset. */
    uint8_t                    sr_key[32];

    /* Per-process AEAD key seeded once via OpenSSL DRBG. Used by
     * ngtcp2_crypto_generate_retry_token / verify_retry_token to
     * authenticate Retry tokens (RFC 9000 §8.1.2). Server restart
     * invalidates outstanding tokens — same trade-off as sr_key. */
    uint8_t                    retry_token_key[32];

    /* Per-peer-IP concurrent-connection budget. HashTable
     * keyed by raw 4-byte (v4) or 16-byte (v6) IP. Value is the live
     * count, stored as a uintptr_t cast (no per-key alloc). Cap is
     * peer_budget; envvar PHP_HTTP3_PEER_BUDGET overrides at spawn.
     * Mitigates handshake slow-loris + amplification by capping the
     * fan-out a single source can pin on the server. */
    HashTable                 *peer_count_map;
    uint32_t                   peer_budget;

    /* Cache for last_peer formatting — inet_ntop + snprintf cost ~100 ns
     * per datagram, but back-to-back packets from the same peer are the
     * common case (handshake + 0-RTT + early app data, or any flooded
     * client). Skip the reformat when the raw sockaddr bytes match what
     * we last printed. Keyed on family+port+addr only; addr_len gates
     * the memcmp. */
    struct sockaddr_storage    last_peer_addr;
    socklen_t                  last_peer_addr_len;
    bool                       last_peer_valid;

    http3_listener_stats_t     stats;

    /* Slab pool for http3_stream_t. Shared across all conns on this
     * listener. Initialised in http3_listener_spawn, cleaned up in
     * http3_listener_destroy after all conns are gone. */
    http3_stream_pool_t        stream_pool;
};

/* Callback subclass — carries a back-pointer to the listener. */
typedef struct {
    zend_async_event_callback_t base;
    http3_listener_t           *listener;
} http3_recv_cb_t;

static void format_peer(const struct sockaddr *addr, socklen_t addr_len,
                        char *out, size_t out_size)
{
    if (addr == NULL || out_size == 0) {
        if (out_size > 0) out[0] = '\0';
        return;
    }

    char ip[INET6_ADDRSTRLEN];
    uint16_t port_be = 0;
    ip[0] = '\0';

    if (addr->sa_family == AF_INET && addr_len >= (socklen_t)sizeof(struct sockaddr_in)) {
        const struct sockaddr_in *sin = (const struct sockaddr_in *)addr;
        inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof(ip));
        port_be = sin->sin_port;
    } else if (addr->sa_family == AF_INET6 && addr_len >= (socklen_t)sizeof(struct sockaddr_in6)) {
        const struct sockaddr_in6 *sin6 = (const struct sockaddr_in6 *)addr;
        inet_ntop(AF_INET6, &sin6->sin6_addr, ip, sizeof(ip));
        port_be = sin6->sin6_port;
    } else {
        snprintf(out, out_size, "unknown");
        return;
    }

    snprintf(out, out_size, "%s:%u", ip, (unsigned)ntohs(port_be));
}

/* True iff `a` and `b` describe the same peer (same family + port + addr).
 * Ignores sockaddr_in.sin_zero / unrelated padding by comparing the named
 * fields directly. */
static bool sockaddr_same_peer(const struct sockaddr *a, socklen_t a_len,
                               const struct sockaddr *b, socklen_t b_len)
{
    if (a == NULL || b == NULL || a->sa_family != b->sa_family) return false;
    if (a->sa_family == AF_INET
        && a_len >= (socklen_t)sizeof(struct sockaddr_in)
        && b_len >= (socklen_t)sizeof(struct sockaddr_in)) {
        const struct sockaddr_in *sa = (const struct sockaddr_in *)a;
        const struct sockaddr_in *sb = (const struct sockaddr_in *)b;
        return sa->sin_port == sb->sin_port
            && sa->sin_addr.s_addr == sb->sin_addr.s_addr;
    }
    if (a->sa_family == AF_INET6
        && a_len >= (socklen_t)sizeof(struct sockaddr_in6)
        && b_len >= (socklen_t)sizeof(struct sockaddr_in6)) {
        const struct sockaddr_in6 *sa = (const struct sockaddr_in6 *)a;
        const struct sockaddr_in6 *sb = (const struct sockaddr_in6 *)b;
        return sa->sin6_port == sb->sin6_port
            && memcmp(&sa->sin6_addr, &sb->sin6_addr, sizeof(sa->sin6_addr)) == 0;
    }
    return false;
}

/* Hot-path peer-cache update — both recv paths share the inet_ntop skip.
 * Caller has already verified addr_len > 0 and addr is non-NULL. */
static void update_peer_cache(http3_listener_t *listener,
                              const struct sockaddr *cur_addr,
                              socklen_t cur_addr_len)
{
    if (!listener->last_peer_valid
        || !sockaddr_same_peer(cur_addr, cur_addr_len,
                               (const struct sockaddr *)&listener->last_peer_addr,
                               listener->last_peer_addr_len)) {
        format_peer(cur_addr, cur_addr_len,
                    listener->stats.last_peer, sizeof(listener->stats.last_peer));
        if ((size_t)cur_addr_len <= sizeof(listener->last_peer_addr)) {
            memcpy(&listener->last_peer_addr, cur_addr, (size_t)cur_addr_len);
            listener->last_peer_addr_len = cur_addr_len;
            listener->last_peer_valid = true;
        }
    }
}

#ifndef __linux__
/* Legacy libuv-wrapped path. Kept for non-Linux builds; on Linux the
 * raw-fd recvmmsg loop below replaces it. */
static void http3_listener_recv_cb(zend_async_event_t *event,
                                   zend_async_event_callback_t *cb,
                                   void *result, zend_object *exception)
{
    (void)event;
    http3_recv_cb_t *rcb = (http3_recv_cb_t *)cb;
    http3_listener_t *listener = rcb->listener;
    zend_async_udp_req_t *req = (zend_async_udp_req_t *)result;

    if (listener == NULL || listener->closed) {
        return;
    }
    if (req == NULL || req != listener->recv_req) {
        return;
    }
    if (exception != NULL || req->exception != NULL) {
        listener->stats.datagrams_errored++;
        return;
    }
    if (req->transferred <= 0) {
        return;
    }

    listener->stats.datagrams_received++;
    listener->stats.bytes_received += (uint64_t)req->transferred;
    listener->stats.last_datagram_size = (size_t)req->transferred;

    update_peer_cache(listener, (const struct sockaddr *)&req->addr, req->addr_len);

    /* Legacy libuv path has no cmsg access — ECN unavailable. */
    http3_connection_dispatch(listener,
                              (const uint8_t *)req->buf, (size_t)req->transferred, 0,
                              (const struct sockaddr *)&req->addr, req->addr_len);
}
#endif /* !__linux__ */

#ifdef __linux__
/* Drain the socket error queue. Async send failures (ICMP unreachable,
 * "frag needed" under DF, NIC-delayed errors) land here, NOT in the
 * sendmsg return value. We drain on every poll wakeup so pending serr
 * cmsgs don't accumulate; counters are exposed via getHttp3Stats().
 *
 * Proper feedback into ngtcp2 (PMTU shrink, connection-dead) is a
 * follow-up — for now we just expose the rate. */
static void drain_err_queue(http3_listener_t *listener)
{
    http3_packet_stats_t *st = &listener->stats.packet;
    /* Bounded so a flood can't pin the reactor. 32 ≫ any normal rate. */
    for (int i = 0; i < 32; ++i) {
        uint8_t   payload[256];   /* echoed packet header — length only matters */
        uint8_t   ctrl[CMSG_SPACE(sizeof(struct sock_extended_err)) + 64];
        struct iovec iov = { .iov_base = payload, .iov_len = sizeof(payload) };
        struct sockaddr_storage origin;
        struct msghdr msg = {
            .msg_name    = &origin,
            .msg_namelen = sizeof(origin),
            .msg_iov     = &iov,
            .msg_iovlen  = 1,
            .msg_control = ctrl,
            .msg_controllen = sizeof(ctrl),
        };
        ssize_t rv;
        do {
            rv = recvmsg(listener->fd, &msg, MSG_ERRQUEUE | MSG_DONTWAIT);
        } while (rv < 0 && errno == EINTR);
        if (rv < 0) return;     /* nothing pending */

        for (struct cmsghdr *cm = CMSG_FIRSTHDR(&msg); cm != NULL;
             cm = CMSG_NXTHDR(&msg, cm)) {
#ifdef IP_RECVERR
            bool is_v4 = (cm->cmsg_level == IPPROTO_IP   && cm->cmsg_type == IP_RECVERR);
#else
            bool is_v4 = false;
#endif
#ifdef IPV6_RECVERR
            bool is_v6 = (cm->cmsg_level == IPPROTO_IPV6 && cm->cmsg_type == IPV6_RECVERR);
#else
            bool is_v6 = false;
#endif
            if (!is_v4 && !is_v6) continue;

            struct sock_extended_err see;
            memcpy(&see, CMSG_DATA(cm), sizeof(see));
            switch (see.ee_errno) {
                case EMSGSIZE:    st->quic_errqueue_emsgsize++; break;
                case EHOSTUNREACH:
                case ENETUNREACH: st->quic_errqueue_unreach++;  break;
                default:          st->quic_errqueue_other++;
            }
        }
    }
}

/* Raw-fd recvmmsg path. Drains up to HTTP3_LISTENER_RECV_BATCH
 * datagrams per recvmmsg syscall, capped at 16 batches per poll wakeup.
 * h2o picks the same 10-batch limit (lib/http3/common.c:819) — a larger
 * batch starves QUIC ACK feedback because all ACKs in the batch are
 * deferred until handler returns. Stack buffers (~16 KiB), zero heap. */
static void http3_listener_poll_cb(zend_async_event_t *event,
                                   zend_async_event_callback_t *cb,
                                   void *result, zend_object *exception)
{
    (void)event;
    (void)result;
    http3_recv_cb_t *rcb = (http3_recv_cb_t *)cb;
    http3_listener_t *listener = rcb->listener;

    if (listener == NULL || listener->closed) {
        return;
    }
    if (exception != NULL) {
        listener->stats.datagrams_errored++;
        return;
    }
    if (listener->fd < 0) {
        return;
    }

    /* Drain the kernel error queue — but only when there is reason to
     * believe something is pending. Most sockets never see ICMP errors,
     * and the unconditional recvmsg(MSG_ERRQUEUE) costs ~10% throughput
     * on a clean fast path. The flag is set by send_packet/send_gso on
     * any sendmsg failure (sync errors correlate with async ICMP). */
    if (UNEXPECTED(listener->errq_pending)) {
        listener->errq_pending = false;
        drain_err_queue(listener);
    }

    /* Per-call cap on the outer drain loop. Bounds CPU under a flood;
     * subsequent batches arrive on the next poll wakeup. 16×10 = 160
     * datagrams/tick — well above any legitimate single-RTT burst. */
    enum { H3_DRAIN_BATCH_CAP = 16 };

    struct mmsghdr mess[HTTP3_LISTENER_RECV_BATCH];
    struct iovec   iovs[HTTP3_LISTENER_RECV_BATCH];
    uint8_t        bufs[HTTP3_LISTENER_RECV_BATCH][HTTP3_LISTENER_DGRAM_SIZE];
    struct sockaddr_storage src_addrs[HTTP3_LISTENER_RECV_BATCH];
    /* cmsg ctrl buffer per mmsghdr — sized for IP_TOS (int via
     * IPV6_TCLASS) which is the largest single cmsg we need on
     * recv. CMSG_SPACE(sizeof(int)) typically ≈ 24 bytes. */
    uint8_t        ctrls[HTTP3_LISTENER_RECV_BATCH][CMSG_SPACE(sizeof(int))];

    for (unsigned outer = 0; outer < H3_DRAIN_BATCH_CAP; ++outer) {
        for (int i = 0; i < HTTP3_LISTENER_RECV_BATCH; ++i) {
            iovs[i].iov_base = bufs[i];
            iovs[i].iov_len  = HTTP3_LISTENER_DGRAM_SIZE;
            mess[i].msg_hdr  = (struct msghdr){
                .msg_name    = &src_addrs[i],
                .msg_namelen = sizeof(src_addrs[i]),
                .msg_iov     = &iovs[i],
                .msg_iovlen  = 1,
                .msg_control = ctrls[i],
                .msg_controllen = sizeof(ctrls[i]),
            };
            mess[i].msg_len = 0;
        }

        int n;
        do {
            n = recvmmsg(listener->fd, mess, HTTP3_LISTENER_RECV_BATCH, 0, NULL);
        } while (n < 0 && errno == EINTR);

        if (n <= 0) {
            /* EAGAIN/EWOULDBLOCK = queue drained. Anything else is a
             * terminal error on this fd; bump the counter and bail —
             * the next poll wakeup will retry. */
            if (n < 0 && errno != EAGAIN
#if EAGAIN != EWOULDBLOCK
                && errno != EWOULDBLOCK
#endif
            ) {
                listener->stats.datagrams_errored++;
            }
            return;
        }

        for (int i = 0; i < n; ++i) {
            const struct sockaddr *src =
                (const struct sockaddr *)&src_addrs[i];
            socklen_t src_len = (socklen_t)mess[i].msg_hdr.msg_namelen;
            size_t dlen = (size_t)mess[i].msg_len;

            if (dlen == 0 || src_len == 0) {
                continue;
            }

            /* Pull ECN out of cmsg. RFC 3168 ECN bits live in the lower
             * 2 of IP_TOS / IPV6_TCLASS — ngtcp2 cares about exactly
             * those bits. We pass the byte through unchanged; pi.ecn
             * downstream will mask. */
            uint8_t ecn = 0;
            for (struct cmsghdr *cm = CMSG_FIRSTHDR(&mess[i].msg_hdr);
                 cm != NULL;
                 cm = CMSG_NXTHDR(&mess[i].msg_hdr, cm)) {
                if (cm->cmsg_level == IPPROTO_IP
#ifdef IP_RECVTOS
# ifdef __APPLE__
                    && cm->cmsg_type == IP_RECVTOS
# else
                    && cm->cmsg_type == IP_TOS
# endif
#else
                    && 0
#endif
                ) {
                    ecn = *(const uint8_t *)CMSG_DATA(cm);
                }
#ifdef IPV6_TCLASS
                else if (cm->cmsg_level == IPPROTO_IPV6
                         && cm->cmsg_type == IPV6_TCLASS) {
                    int tclass = 0;
                    memcpy(&tclass, CMSG_DATA(cm), sizeof(tclass));
                    ecn = (uint8_t)(tclass & 0xff);
                }
#endif
            }

            listener->stats.datagrams_received++;
            listener->stats.bytes_received += (uint64_t)dlen;
            listener->stats.last_datagram_size = dlen;

            update_peer_cache(listener, src, src_len);

            http3_connection_dispatch(listener,
                                      bufs[i], dlen, ecn,
                                      src, src_len);

            /* Dispatch may have closed the listener via reap-on-close.
             * Bail out of both loops in that case. */
            if (listener->closed) {
                return;
            }
        }

        /* Short read = queue empty. Avoid the next syscall when we already
         * know there is nothing pending. */
        if (n < HTTP3_LISTENER_RECV_BATCH) {
            return;
        }
    }
}
#endif /* __linux__ */

/* Accessors used by http3_packet.c without exposing the private struct
 * layout. Keeps http3_listener_t opaque outside this TU. */
http3_packet_stats_t *http3_listener_packet_stats(http3_listener_t *l)
{
    return l != NULL ? &l->stats.packet : NULL;
}

zend_async_io_t *http3_listener_io(http3_listener_t *l)
{
    /* Linux raw-fd path returns NULL — callers must use the
     * http3_listener_send_packet helper to send. */
    return l != NULL ? l->udp_io : NULL;
}

#ifndef PHP_WIN32
/* Map a sendmsg errno to the matching counter so callers can drop the
 * `(void)` and we still get observability. Counts on `stats` only when
 * non-NULL; safe to call on every send. Linux/POSIX-only — Windows
 * goes through libuv (zend_async_udp_*) which already carries its own
 * error path; we don't have a raw errno on that path. */
static void account_send_error(http3_packet_stats_t *st, int err)
{
    if (st == NULL) return;
    switch (err) {
        case EAGAIN:
#if EAGAIN != EWOULDBLOCK
        case EWOULDBLOCK:
#endif
            st->quic_send_eagain++;     break;
        case EMSGSIZE:
            st->quic_send_emsgsize++;   break;
        case EHOSTUNREACH:
        case ENETUNREACH:
        case EHOSTDOWN:
        case ENETDOWN:
            st->quic_send_unreach++;    break;
        default:
            st->quic_send_other_error++;
    }
}
#endif

ssize_t http3_listener_send_packet(http3_listener_t *l,
                                   const void *buf, size_t len, uint8_t ecn,
                                   const struct sockaddr *peer,
                                   socklen_t peer_len)
{
    if (l == NULL || buf == NULL || peer == NULL || len == 0) {
        return -EINVAL;
    }

#ifdef __linux__
    if (l->fd >= 0) {
        /* cmsg buffer: only IP_TOS / IPV6_TCLASS, both fit in
         * CMSG_SPACE(sizeof(int)). Zero-initialised so CMSG_NXTHDR is
         * well-defined when we don't push anything. */
        char ctrl[CMSG_SPACE(sizeof(int))];
        memset(ctrl, 0, sizeof(ctrl));
        struct iovec iov = { .iov_base = (void *)buf, .iov_len = len };
        struct msghdr msg = {
            .msg_name    = (void *)peer,
            .msg_namelen = peer_len,
            .msg_iov     = &iov,
            .msg_iovlen  = 1,
            .msg_control = ecn ? ctrl : NULL,
            .msg_controllen = ecn ? sizeof(ctrl) : 0,
        };
        if (ecn) {
            struct cmsghdr *cm = CMSG_FIRSTHDR(&msg);
            int tos = ecn;
            if (peer->sa_family == AF_INET6) {
#ifdef IPV6_TCLASS
                cm->cmsg_level = IPPROTO_IPV6;
                cm->cmsg_type  = IPV6_TCLASS;
                cm->cmsg_len   = CMSG_LEN(sizeof(tos));
                memcpy(CMSG_DATA(cm), &tos, sizeof(tos));
#endif
            } else {
                cm->cmsg_level = IPPROTO_IP;
                cm->cmsg_type  = IP_TOS;
                cm->cmsg_len   = CMSG_LEN(sizeof(tos));
                memcpy(CMSG_DATA(cm), &tos, sizeof(tos));
            }
        }
        ssize_t rv;
        do {
            rv = sendmsg(l->fd, &msg, MSG_DONTWAIT);
        } while (rv < 0 && errno == EINTR);
        if (rv >= 0) {
            return rv;
        }
        account_send_error(&l->stats.packet, errno);
        l->errq_pending = true;
        return -errno;
    }
#endif

    /* Legacy libuv path — used on Windows/macOS and any other non-Linux
     * platform. TrueAsync's UDP API has no synchronous best-effort send
     * (`ZEND_ASYNC_UDP_TRY_SEND` was sketched in the Step-9 audit but
     * never landed); we fire-and-forget through ZEND_ASYNC_UDP_SENDTO
     * which queues to libuv's uv_udp_send. UDP is all-or-nothing per
     * datagram, so reporting `len` bytes accepted is correct as long as
     * the request object initialised. */
    if (l->udp_io == NULL) {
        return -EBADF;
    }
    zend_async_udp_req_t *req = ZEND_ASYNC_UDP_SENDTO(
        l->udp_io, (const char *)buf, len, peer, peer_len);
    if (req == NULL) {
        return -EIO;
    }
    if (req->dispose != NULL) {
        req->dispose(req);
    }
    return (ssize_t)len;
}

ssize_t http3_listener_send_gso(http3_listener_t *l,
                                const void *buf, size_t total_len,
                                size_t segsize, uint8_t ecn,
                                const struct sockaddr *peer,
                                socklen_t peer_len)
{
    if (l == NULL || buf == NULL || peer == NULL || total_len == 0) {
        return -EINVAL;
    }
    if (segsize == 0 || total_len <= segsize) {
        /* Single segment — GSO has no benefit, plain sendmsg. */
        return http3_listener_send_packet(l, buf, total_len, ecn, peer, peer_len);
    }

#if defined(__linux__) && defined(UDP_SEGMENT)
    /* Once GSO has been refused by the kernel/NIC for this socket, skip
     * the cmsg path entirely — every subsequent attempt would just
     * round-trip into the kernel and back with the same EIO/EINVAL. */
    if (l->fd >= 0 && !l->gso_disabled) {
        /* cmsg buffer: UDP_SEGMENT (uint16) + IP_TOS/IPV6_TCLASS (int).
         * Both pushed on the same sendmsg when ecn != 0. */
        char ctrl[CMSG_SPACE(sizeof(uint16_t)) + CMSG_SPACE(sizeof(int))];
        memset(ctrl, 0, sizeof(ctrl));
        struct iovec iov = { .iov_base = (void *)buf, .iov_len = total_len };
        struct msghdr msg = {
            .msg_name    = (void *)peer,
            .msg_namelen = peer_len,
            .msg_iov     = &iov,
            .msg_iovlen  = 1,
            .msg_control = ctrl,
            .msg_controllen = sizeof(ctrl),
        };
        struct cmsghdr *cm = CMSG_FIRSTHDR(&msg);
        cm->cmsg_level = SOL_UDP;
        cm->cmsg_type  = UDP_SEGMENT;
        cm->cmsg_len   = CMSG_LEN(sizeof(uint16_t));
        uint16_t segsize_u16 = (uint16_t)segsize;
        memcpy(CMSG_DATA(cm), &segsize_u16, sizeof(segsize_u16));

        size_t actual_ctrl = CMSG_SPACE(sizeof(uint16_t));
        if (ecn) {
            struct cmsghdr *cm2 = CMSG_NXTHDR(&msg, cm);
            int tos = ecn;
            if (peer->sa_family == AF_INET6) {
#ifdef IPV6_TCLASS
                cm2->cmsg_level = IPPROTO_IPV6;
                cm2->cmsg_type  = IPV6_TCLASS;
                cm2->cmsg_len   = CMSG_LEN(sizeof(tos));
                memcpy(CMSG_DATA(cm2), &tos, sizeof(tos));
                actual_ctrl += CMSG_SPACE(sizeof(tos));
#endif
            } else {
                cm2->cmsg_level = IPPROTO_IP;
                cm2->cmsg_type  = IP_TOS;
                cm2->cmsg_len   = CMSG_LEN(sizeof(tos));
                memcpy(CMSG_DATA(cm2), &tos, sizeof(tos));
                actual_ctrl += CMSG_SPACE(sizeof(tos));
            }
        }
        msg.msg_controllen = actual_ctrl;

        ssize_t rv;
        do {
            rv = sendmsg(l->fd, &msg, MSG_DONTWAIT);
        } while (rv < 0 && errno == EINTR);

        if (rv >= 0) {
            return rv;
        }
        if (errno == EAGAIN
#if EAGAIN != EWOULDBLOCK
            || errno == EWOULDBLOCK
#endif
        ) {
            return -EAGAIN;
        }
        /* EIO = NIC declined TX-UDP-segmentation; EINVAL = kernel
         * disagreed with our buffer layout. Both are persistent for
         * this fd: latch GSO off and bump the counter. The retry below
         * via per-segment sendmsg will deliver the bytes. */
        if (errno == EIO || errno == EINVAL) {
            if (!l->gso_disabled) {
                l->gso_disabled = true;
                http3_packet_stats_t *st = &l->stats.packet;
                st->quic_gso_disabled = 1;
            }
            l->stats.packet.quic_send_gso_refused++;
        }
        l->errq_pending = true;
    }
#endif

    /* Fallback: send each segment as its own datagram. Used both when
     * GSO is unavailable and after the kernel refused it once. */
    const uint8_t *p = buf;
    size_t remaining = total_len;
    while (remaining > 0) {
        size_t this_len = remaining > segsize ? segsize : remaining;
        ssize_t s = http3_listener_send_packet(l, p, this_len, ecn, peer, peer_len);
        if (s < 0 && s != -EAGAIN) {
            return s;
        }
        p += this_len;
        remaining -= this_len;
    }
    return (ssize_t)total_len;
}

void *http3_listener_ssl_ctx(http3_listener_t *l)
{
    return l != NULL ? l->ssl_ctx : NULL;
}

void *http3_listener_server_obj(const http3_listener_t *l)
{
    return l != NULL ? l->server_obj : NULL;
}

const uint8_t *http3_listener_sr_key(const http3_listener_t *l)
{
    return l != NULL ? l->sr_key : NULL;
}

const uint8_t *http3_listener_retry_token_key(const http3_listener_t *l)
{
    return l != NULL ? l->retry_token_key : NULL;
}

/* Link a fresh connection onto the listener's ownership list. Called
 * by http3_connection_accept after the ngtcp2_conn is ready; caller
 * still has to register the connection in conn_map under whichever
 * lookup keys it wants. */
void http3_listener_track_connection(http3_listener_t *l,
                                     http3_connection_t *conn)
{
    if (l == NULL || conn == NULL) {
        return;
    }
    conn->next = l->conn_list;
    l->conn_list = conn;
}

/* Per-peer budget helpers.
 *
 * Key is the raw IP bytes (4 for v4, 16 for v6); the source port is
 * intentionally NOT part of the key, so a single client behind NAT
 * cannot bypass the cap by spreading across ephemeral ports. */
static bool peer_key_from_sockaddr(const struct sockaddr *peer,
                                   uint8_t out[16], size_t *out_len)
{
    if (peer == NULL) return false;
    if (peer->sa_family == AF_INET) {
        memcpy(out, &((const struct sockaddr_in *)peer)->sin_addr, 4);
        *out_len = 4;
        return true;
    }
    if (peer->sa_family == AF_INET6) {
        memcpy(out, &((const struct sockaddr_in6 *)peer)->sin6_addr, 16);
        *out_len = 16;
        return true;
    }
    return false;
}

bool http3_listener_peer_inc(http3_listener_t *l,
                             const struct sockaddr *peer)
{
    if (l == NULL || peer == NULL) return true;       /* fail-open */
    uint8_t key[16]; size_t klen = 0;
    if (!peer_key_from_sockaddr(peer, key, &klen)) {
        return true;                                   /* unknown family */
    }
    if (l->peer_count_map == NULL) {
        ALLOC_HASHTABLE(l->peer_count_map);
        zend_hash_init(l->peer_count_map, 16, NULL, NULL, 0);
    }
    void *p = zend_hash_str_find_ptr(l->peer_count_map,
                                     (const char *)key, klen);
    uintptr_t cnt = (uintptr_t)p;
    if (cnt >= l->peer_budget) {
        return false;
    }
    cnt++;
    zend_hash_str_update_ptr(l->peer_count_map,
                             (const char *)key, klen, (void *)cnt);
    return true;
}

void http3_listener_peer_dec(http3_listener_t *l,
                             const struct sockaddr *peer)
{
    if (l == NULL || peer == NULL || l->peer_count_map == NULL) return;
    uint8_t key[16]; size_t klen = 0;
    if (!peer_key_from_sockaddr(peer, key, &klen)) return;
    void *p = zend_hash_str_find_ptr(l->peer_count_map,
                                     (const char *)key, klen);
    if (p == NULL) return;
    uintptr_t cnt = (uintptr_t)p;
    if (cnt <= 1) {
        zend_hash_str_del(l->peer_count_map, (const char *)key, klen);
    } else {
        cnt--;
        zend_hash_str_update_ptr(l->peer_count_map,
                                 (const char *)key, klen, (void *)cnt);
    }
}

/* Unhook a connection from the listener's intrusive list and
 * remove every key it occupies in conn_map. The map is non-owning so
 * removing the keys does NOT free the connection; the caller does that.
 * No-op if the connection is not actually attached. Safe to call before
 * http3_connection_free as part of the reap path. */
void http3_listener_remove_connection(http3_listener_t *l,
                                      http3_connection_t *conn)
{
    if (l == NULL || conn == NULL) {
        return;
    }

    /* Singly-linked unhook. Conn count is bounded by max_connections
     * (low thousands) and reap is rare relative to packet flow, so the
     * O(N) walk is fine. */
    if (l->conn_list == conn) {
        l->conn_list = conn->next;
    } else {
        for (http3_connection_t *p = l->conn_list; p != NULL; p = p->next) {
            if (p->next == conn) {
                p->next = conn->next;
                break;
            }
        }
    }
    conn->next = NULL;

    if (l->conn_map != NULL) {
        if (conn->scidlen > 0) {
            zend_hash_str_del(l->conn_map,
                (const char *)conn->scid, conn->scidlen);
        }
        if (conn->original_dcidlen > 0
            && (conn->original_dcidlen != conn->scidlen
                || memcmp(conn->original_dcid, conn->scid, conn->scidlen) != 0)) {
            zend_hash_str_del(l->conn_map,
                (const char *)conn->original_dcid, conn->original_dcidlen);
        }
    }
}

HashTable *http3_listener_conn_map(http3_listener_t *l)
{
    if (l == NULL) {
        return NULL;
    }
    /* Lazily allocate so idle listeners (no QUIC connections accepted)
     * do not pay for hashtable init. Zend's HashTable allocates its
     * table lazily too —
     * the wrapper is 56 bytes + nothing until first insert. */
    if (l->conn_map == NULL) {
        ALLOC_HASHTABLE(l->conn_map);
        zend_hash_init(l->conn_map, 16, NULL, NULL, /* persistent: */ 0);
    }
    return l->conn_map;
}

/* Eagerly initialise ngtcp2's OpenSSL provider hooks. ngtcp2 docs are
 * silent on ordering, but empirically a stuck TLS state machine where
 * the server processes the ClientHello but never produces ServerHello
 * is the result of running this *after* SSL_set_quic_tls_cbs; making
 * it happen before any SSL-side configuration eliminates that race
 * (the symptom: every reply is an Initial-with-ACK, no CRYPTO). */
extern int ngtcp2_crypto_ossl_init(void);

http3_listener_t *http3_listener_spawn(const char *host, int port,
                                       void *ssl_ctx, void *server_obj)
{
    /* Belt-and-braces: even if connection_attach_tls also calls this,
     * doing it once at listener-spawn time guarantees the provider is
     * registered before the first datagram lands. */
    (void)ngtcp2_crypto_ossl_init();

    http3_listener_t *listener = ecalloc(1, sizeof(http3_listener_t));
    listener->fd         = -1;
    listener->host       = estrdup(host);
    listener->port       = port;
    listener->ssl_ctx    = ssl_ctx;
    listener->server_obj = server_obj;
    http3_stream_pool_init(&listener->stream_pool);

#ifdef __linux__
    /* Raw-fd path: socket() + setsockopt + bind() + uv_poll_t. We need
     * cmsg access for UDP_SEGMENT (GSO), IP_TOS (ECN), IP_RECVERR (async
     * send errors) — none of which libuv exposes. The libuv maintainer's
     * recommended workaround for this exact case is to open the fd
     * yourself and pass it into uv_poll (libuv discussion #3348). */
    int family = AF_UNSPEC;
    struct sockaddr_storage bind_addr;
    socklen_t bind_addr_len = 0;
    if (strchr(host, ':') != NULL) {
        family = AF_INET6;
        struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)&bind_addr;
        memset(sin6, 0, sizeof(*sin6));
        sin6->sin6_family = AF_INET6;
        sin6->sin6_port = htons((uint16_t)port);
        if (inet_pton(AF_INET6, host, &sin6->sin6_addr) != 1) {
            zend_throw_error(NULL, "HTTP/3 listener: invalid IPv6 address %s", host);
            http3_listener_destroy(listener);
            return NULL;
        }
        bind_addr_len = (socklen_t)sizeof(*sin6);
    } else {
        family = AF_INET;
        struct sockaddr_in *sin = (struct sockaddr_in *)&bind_addr;
        memset(sin, 0, sizeof(*sin));
        sin->sin_family = AF_INET;
        sin->sin_port = htons((uint16_t)port);
        if (inet_pton(AF_INET, host, &sin->sin_addr) != 1) {
            zend_throw_error(NULL, "HTTP/3 listener: invalid IPv4 address %s", host);
            http3_listener_destroy(listener);
            return NULL;
        }
        bind_addr_len = (socklen_t)sizeof(*sin);
    }

    int fd = socket(family, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        zend_throw_error(NULL,
            "HTTP/3 listener: socket() failed: %s", strerror(errno));
        http3_listener_destroy(listener);
        return NULL;
    }
    listener->fd = fd;
    listener->family = family;

    /* Match the libuv default plus what we need for QUIC. */
    int one = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#ifdef SO_REUSEPORT
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
#endif
    if (family == AF_INET6) {
        (void)setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &one, sizeof(one));
#ifdef IPV6_RECVTCLASS
        (void)setsockopt(fd, IPPROTO_IPV6, IPV6_RECVTCLASS, &one, sizeof(one));
#endif
#ifdef IPV6_RECVERR
        /* Enable extended-error reporting so ICMP send failures land
         * in MSG_ERRQUEUE for our drain_err_queue() pass. */
        (void)setsockopt(fd, IPPROTO_IPV6, IPV6_RECVERR, &one, sizeof(one));
#endif
#if defined(IPV6_MTU_DISCOVER) && defined(IPV6_PMTUDISC_DO)
        {
            int pmtud = IPV6_PMTUDISC_DO;
            (void)setsockopt(fd, IPPROTO_IPV6, IPV6_MTU_DISCOVER,
                             &pmtud, sizeof(pmtud));
        }
#endif
    } else {
#ifdef IP_RECVTOS
        (void)setsockopt(fd, IPPROTO_IP, IP_RECVTOS, &one, sizeof(one));
#endif
#ifdef IP_RECVERR
        (void)setsockopt(fd, IPPROTO_IP, IP_RECVERR, &one, sizeof(one));
#endif
#if defined(IP_MTU_DISCOVER) && defined(IP_PMTUDISC_DO)
        {
            int pmtud = IP_PMTUDISC_DO;
            (void)setsockopt(fd, IPPROTO_IP, IP_MTU_DISCOVER,
                             &pmtud, sizeof(pmtud));
        }
#endif
    }

    if (bind(fd, (struct sockaddr *)&bind_addr, bind_addr_len) < 0) {
        zend_throw_error(NULL,
            "HTTP/3 listener: bind to %s:%d failed: %s",
            host, port, strerror(errno));
        http3_listener_destroy(listener);
        return NULL;
    }
#else
    if (zend_async_udp_bind_fn == NULL) {
        zend_throw_error(NULL,
            "HTTP/3 listener requires a reactor that implements zend_async_udp_bind "
            "(missing from this TrueAsync build)");
        http3_listener_destroy(listener);
        return NULL;
    }

    /* SO_REUSEPORT is Linux-only (libuv maps it to UV_UDP_REUSEPORT, which
     * requires Linux >= 3.9). Winsock returns WSAEOPNOTSUPP at bind time
     * if the flag is set on Windows; libuv on macOS forwards SO_REUSEPORT
     * to the BSD-style "permit overlapping wildcard binds" semantics, not
     * the load-balancing fan-out we want — close enough that we still ask
     * for it there. */
    unsigned int bind_flags = 0;
#ifndef PHP_WIN32
    bind_flags |= ZEND_ASYNC_UDP_F_REUSEPORT;
#endif
    zend_async_io_t *udp_io = ZEND_ASYNC_UDP_BIND_EX(host, port, bind_flags, 0);
    if (udp_io == NULL) {
        /* ZEND_ASYNC_UDP_BIND_EX has already thrown a descriptive async
         * exception (uv_strerror) — propagate it. */
        http3_listener_destroy(listener);
        return NULL;
    }
    listener->udp_io = udp_io;
#endif

    /* Seed the stateless-reset HMAC key. Failure is fatal:
     * a zero-key would let any peer forge tokens against any DCID. */
    if (RAND_bytes(listener->sr_key, sizeof(listener->sr_key)) != 1) {
        zend_throw_error(NULL,
            "HTTP/3 listener: OpenSSL RAND_bytes failed seeding "
            "stateless-reset key");
        http3_listener_destroy(listener);
        return NULL;
    }

    /* Seed the Retry-token AEAD key. Same fatal-on-failure policy:
     * a predictable key lets attackers forge Retry tokens, defeating
     * source-address validation. */
    if (RAND_bytes(listener->retry_token_key,
                   sizeof(listener->retry_token_key)) != 1) {
        zend_throw_error(NULL,
            "HTTP/3 listener: OpenSSL RAND_bytes failed seeding "
            "Retry token key");
        http3_listener_destroy(listener);
        return NULL;
    }

    /* Per-peer budget. Resolution order:
     *   1. HttpServerConfig::setHttp3PeerConnectionBudget() at start().
     *      Reads via http_server_get_http3_peer_connection_budget;
     *      0 means "config left unset" → fall through.
     *   2. PHP_HTTP3_PEER_BUDGET env (legacy ops escape hatch).
     *   3. Built-in default 16 (covers polite browser fan-out for a
     *      single user behind NAT). */
    listener->peer_budget = 16;
    uint32_t cfg_budget = http_server_get_http3_peer_connection_budget(
        (const http_server_object *)server_obj);
    if (cfg_budget != 0) {
        listener->peer_budget = cfg_budget;
    }
    {
        const char *env = getenv("PHP_HTTP3_PEER_BUDGET");
        if (env != NULL && *env != '\0') {
            char *end = NULL;
            unsigned long n = strtoul(env, &end, 10);
            if (end != env && *end == '\0' && n > 0 && n <= 4096) {
                listener->peer_budget = (uint32_t)n;
                /* A silent env-var override of a security knob is
                 * exactly the config drift we want operators to see. */
                http_logf_info(
                    http_server_get_log_state((http_server_object *)server_obj),
                    "h3.listener.peer_budget.override env=%lu config_default=%u",
                    n, (unsigned)cfg_budget);
            } else {
                http_logf_info(
                    http_server_get_log_state((http_server_object *)server_obj),
                    "h3.listener.peer_budget.ignored value='%s' "
                    "expected_range='(0, 4096]'",
                    env);
            }
        }
    }

#ifdef __linux__
    /* Wrap the raw fd in a uv_poll_t-equivalent so we get notified on
     * readable. ZEND_ASYNC_NEW_POLL_EVENT calls uv_poll_init_socket +
     * (when start fires) uv_poll_start. Multi-callback semantics let
     * us drain via recvmmsg in the callback. */
    listener->poll_event = ZEND_ASYNC_NEW_POLL_EVENT(
        ZEND_FD_NULL, (zend_socket_t)listener->fd, ASYNC_READABLE);
    if (listener->poll_event == NULL) {
        http3_listener_destroy(listener);
        return NULL;
    }

    /* Tell the poll-event close handler to close the fd for us when the
     * event ref reaches 0. We hold the fd-owning bit here. */
    ZEND_ASYNC_EVENT_SET_CLOSE_FD(&listener->poll_event->base);

    http3_recv_cb_t *rcb = (http3_recv_cb_t *)ZEND_ASYNC_EVENT_CALLBACK_EX(
        http3_listener_poll_cb, sizeof(http3_recv_cb_t));
    if (rcb == NULL) {
        http3_listener_destroy(listener);
        return NULL;
    }
    rcb->listener = listener;
    listener->poll_cb = &rcb->base;

    if (!listener->poll_event->base.add_callback(&listener->poll_event->base,
                                                  &rcb->base)) {
        ZEND_ASYNC_EVENT_CALLBACK_RELEASE(&rcb->base);
        listener->poll_cb = NULL;
        http3_listener_destroy(listener);
        return NULL;
    }

    listener->poll_event->base.start(&listener->poll_event->base);
#else
    /* Stay armed across datagrams — one recvfrom submit, continuous delivery
     * into the callback. libuv_udp_recvfrom starts uv_udp_recv_start, which
     * runs indefinitely under multishot semantics. */
    ZEND_ASYNC_IO_SET_MULTISHOT(udp_io);

    http3_recv_cb_t *rcb = (http3_recv_cb_t *)ZEND_ASYNC_EVENT_CALLBACK_EX(
        http3_listener_recv_cb, sizeof(http3_recv_cb_t));
    if (rcb == NULL) {
        http3_listener_destroy(listener);
        return NULL;
    }
    rcb->listener = listener;
    listener->recv_cb = &rcb->base;

    if (!udp_io->event.add_callback(&udp_io->event, &rcb->base)) {
        ZEND_ASYNC_EVENT_CALLBACK_RELEASE(&rcb->base);
        listener->recv_cb = NULL;
        http3_listener_destroy(listener);
        return NULL;
    }

    zend_async_udp_req_t *req = ZEND_ASYNC_UDP_RECVFROM(udp_io, 2048);
    if (req == NULL) {
        http3_listener_destroy(listener);
        return NULL;
    }
    listener->recv_req = req;
#endif

    return listener;
}

void http3_listener_get_stats(const http3_listener_t *listener,
                              http3_listener_stats_t *out)
{
    if (listener == NULL || out == NULL) {
        return;
    }
    *out = listener->stats;
}

const char *http3_listener_host(const http3_listener_t *listener)
{
    return listener ? listener->host : NULL;
}

int http3_listener_port(const http3_listener_t *listener)
{
    return listener ? listener->port : 0;
}

void http3_listener_destroy(http3_listener_t *listener)
{
    if (listener == NULL || listener->closed) {
        return;
    }
    listener->closed = true;

    /* 1. Sever the callback's back-pointer to our listener data BEFORE
     *    touching the io. Any recv_cb invocation that the reactor has
     *    already queued (or runs during uv_close's teardown tick) reads
     *    listener == NULL and becomes a no-op. */
    if (listener->recv_cb != NULL) {
        ((http3_recv_cb_t *)listener->recv_cb)->listener = NULL;
    }

    /* 2. Free QUIC connections FIRST while the UDP socket is still open.
     *    Each connection's free path disarms its retransmission timer and
     *    tears down ngtcp2_conn; a future iteration that sends CONNECTION_
     *    CLOSE needs the io alive for the last outbound datagram. Order:
     *    conns → io, not io → conns.
     *
     *    We walk the intrusive conn_list — NOT conn_map — because the
     *    same connection appears in the hashtable under multiple keys
     *    (server SCID + client original DCID) and iterating the map
     *    would visit it twice and UAF on the second free. */
    while (listener->conn_list != NULL) {
        http3_connection_t *conn = listener->conn_list;
        listener->conn_list = conn->next;
        conn->next = NULL;
        http3_connection_free(conn);
    }
    if (listener->conn_map != NULL) {
        zend_hash_destroy(listener->conn_map);
        FREE_HASHTABLE(listener->conn_map);
        listener->conn_map = NULL;
    }
    if (listener->peer_count_map != NULL) {
        zend_hash_destroy(listener->peer_count_map);
        FREE_HASHTABLE(listener->peer_count_map);
        listener->peer_count_map = NULL;
    }

    /* 3. Close the IO source. On Linux that is the uv_poll_t wrapping
     *    our raw fd; on other platforms the libuv-wrapped UDP handle. */
#ifdef __linux__
    if (listener->poll_event != NULL) {
        zend_async_poll_event_t *pe = listener->poll_event;
        listener->poll_event = NULL;
        listener->poll_cb = NULL;
        /* Stop polling, then dispose. dispose schedules uv_close which
         * will run libuv_close_poll_handle_cb on the next tick — that
         * callback honours ZEND_ASYNC_EVENT_F_CLOSE_FD and closes our
         * fd. So we must NOT close(listener->fd) ourselves. */
        pe->base.stop(&pe->base);
        pe->base.dispose(&pe->base);
        listener->fd = -1;
    } else if (listener->fd >= 0) {
        /* Spawn failed before poll_event was created — close fd directly. */
        close(listener->fd);
        listener->fd = -1;
    }
#endif
    if (listener->udp_io != NULL) {
        zend_async_io_t *io = listener->udp_io;
        listener->udp_io = NULL;
        listener->recv_cb = NULL;
        listener->recv_req = NULL;
        ZEND_ASYNC_IO_CLOSE(io);
        io->event.dispose(&io->event);
    }

    if (listener->host != NULL) {
        efree(listener->host);
        listener->host = NULL;
    }

    /* Wipe the SR key before returning to the heap; an attacker
     * with UAF read on the listener struct would otherwise be able to
     * forge stateless-reset tokens for any future connection from this
     * process. The compiler can't dead-store-eliminate OPENSSL_cleanse. */
    OPENSSL_cleanse(listener->sr_key, sizeof(listener->sr_key));
    OPENSSL_cleanse(listener->retry_token_key, sizeof(listener->retry_token_key));

    /* Stream pool: every conn freed above released its streams back into
     * the pool, so live_count is 0 here. cleanup() walks the chunk chain
     * and pefrees the slab memory. */
    http3_stream_pool_cleanup(&listener->stream_pool);

    efree(listener);
}

http3_stream_pool_t *http3_listener_stream_pool(http3_listener_t *listener)
{
    return listener != NULL ? &listener->stream_pool : NULL;
}

