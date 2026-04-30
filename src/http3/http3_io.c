/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+

  HTTP/3 packet & timer machinery.

  Split out of http3_connection.c per audit #8. Owns:
    - the per-connection retransmission/idle timer (arm/detach/fire);
    - the drain_out send loop (ngtcp2 + nghttp3 → GSO sendmsg batch);
    - emit_close (graceful CONNECTION_CLOSE);
    - reap + post-IO terminal-state probe.
*/

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif


#include "http3_internal.h"   /* php.h + Zend/zend_async_API.h + ngtcp2 +
                                * nghttp3 + openssl/ssl.h + http3_connection.h */
#include "http3_listener.h"
#include "http3_packet.h"

#include <string.h>

/* sockaddr_storage is reachable transitively, but the GSO batch path also
 * touches struct sockaddr_in6 directly — pull the right networking
 * headers explicitly so this TU doesn't depend on include order. */
#ifdef PHP_WIN32
# include <winsock2.h>
# include <ws2tcpip.h>
#else
# include <sys/socket.h>
# include <netinet/in.h>
#endif

/* Listener accessors implemented in http3_listener.c. */
extern http3_packet_stats_t *http3_listener_packet_stats(http3_listener_t *l);
extern void http3_listener_remove_connection(
    http3_listener_t *l, http3_connection_t *conn);

/* ------------------------------------------------------------------------
 * Timer
 * ------------------------------------------------------------------------ */

typedef struct {
    zend_async_event_callback_t base;
    http3_connection_t         *conn;
} http3_timer_cb_t;

static void timer_fire_cb(zend_async_event_t *event,
                          zend_async_event_callback_t *cb,
                          void *result, zend_object *exception)
{
    (void)event; (void)result; (void)exception;
    http3_timer_cb_t *tcb = (http3_timer_cb_t *)cb;
    http3_connection_t *c = tcb->conn;
    if (c == NULL || c->closed) {
        return;
    }

    http3_packet_stats_t *stats = http3_listener_packet_stats(c->listener);
    if (stats != NULL) {
        stats->quic_timer_fired++;
    }

    int rv = ngtcp2_conn_handle_expiry((ngtcp2_conn *)c->ngtcp2_conn, http3_ts_now());
    /* Idle-timeout: peer is gone, RFC 9000 §10.1 says do NOT emit
     * CONNECTION_CLOSE. Mark sent_connection_close so the teardown also
     * skips the emit, and reap immediately. */
    if (rv == NGTCP2_ERR_IDLE_CLOSE) {
        if (stats != NULL) stats->quic_conn_idle_closed++;
        c->sent_connection_close = true;
        http3_connection_reap(c);
        return;
    }
    /* handshake_timeout fired before TLS finished. ngtcp2 has
     * already moved the conn into the closing period; emit the close
     * (transport NO_ERROR — nghttp3 isn't up yet) and reap. */
    if (rv == NGTCP2_ERR_HANDSHAKE_TIMEOUT) {
        if (stats != NULL) stats->quic_conn_handshake_timeout++;
        http3_connection_emit_close(c);
        http3_connection_reap(c);
        return;
    }
    /* Drain unconditionally on other returns: even on a terminal error,
     * ngtcp2 may have produced a CONNECTION_CLOSE frame that the peer
     * needs to observe. Re-arm the timer only while non-terminal. */
    http3_connection_drain_out(c);
    /* If ngtcp2 is now in closing/draining, reap; otherwise rearm. */
    if (http3_connection_check_terminal(c)) {
        return;
    }
    if (rv == 0) {
        http3_connection_arm_timer(c);
    }
}

void http3_connection_detach_timer(http3_connection_t *c)
{
    if (c->timer != NULL) {
        /* Creator-ref release. The timer stops itself naturally on one-
         * shot fire; if it has not fired yet we cancel via stop + dispose.
         * dispose follows the TrueAsync refcounted pattern — the event
         * drops its creator ref, then frees when last consumer releases. */
        if (c->timer_cb != NULL) {
            c->timer->del_callback(c->timer, c->timer_cb);
            c->timer_cb = NULL;
        }
        c->timer->stop(c->timer);
        c->timer->dispose(c->timer);
        c->timer = NULL;
    }
}

void http3_connection_arm_timer(http3_connection_t *c)
{
    if (c == NULL || c->closed) {
        return;
    }

    ngtcp2_tstamp expiry = ngtcp2_conn_get_expiry((ngtcp2_conn *)c->ngtcp2_conn);
    if (expiry == UINT64_MAX) {
        /* ngtcp2 has nothing scheduled — drop any stale timer. */
        http3_connection_detach_timer(c);
        return;
    }

    ngtcp2_tstamp now = http3_ts_now();
    uint64_t delay_ns = (expiry > now) ? (expiry - now) : 1; /* fire ASAP */
    zend_ulong ms = (zend_ulong)(delay_ns / 1000000ULL);
    zend_ulong ns_rem = (zend_ulong)(delay_ns % 1000000ULL);

    /* Hot path: same timer slot, just rearm with the new expiry. The
     * MULTISHOT flag set at first-create stops the reactor from auto-
     * closing the event after each fire, so the same uv_timer_t +
     * callback wrapper survive across cycles. Saves new_timer_event +
     * uv_close + 2 allocs per drain. */
    if (c->timer != NULL && zend_async_timer_rearm_fn != NULL) {
        if (ZEND_ASYNC_TIMER_REARM((zend_async_timer_event_t *)c->timer, ms, ns_rem)) {
            return;
        }
        /* Rearm failed (closed/backend error) — fall through to recreate. */
        http3_connection_detach_timer(c);
    } else if (c->timer != NULL) {
        /* Reactor without rearm support — fall back to detach + recreate. */
        http3_connection_detach_timer(c);
    }

    zend_async_event_t *timer =
        (zend_async_event_t *)ZEND_ASYNC_NEW_TIMER_EVENT_NS(ms, ns_rem, /*periodic*/ false);
    if (timer == NULL) {
        return;
    }
    /* Opt into the no-auto-close-on-fire lifetime so future arm cycles
     * can reuse this slot via rearm. We dispose explicitly in
     * http3_connection_detach_timer at teardown. */
    ZEND_ASYNC_TIMER_SET_MULTISHOT((zend_async_timer_event_t *)timer);

    http3_timer_cb_t *tcb = (http3_timer_cb_t *)ZEND_ASYNC_EVENT_CALLBACK_EX(
        timer_fire_cb, sizeof(http3_timer_cb_t));
    if (UNEXPECTED(tcb == NULL)) {
        timer->dispose(timer);
        return;
    }
    tcb->conn = c;

    if (UNEXPECTED(!timer->add_callback(timer, &tcb->base))) {
        ZEND_ASYNC_EVENT_CALLBACK_RELEASE(&tcb->base);
        timer->dispose(timer);
        return;
    }

    c->timer    = timer;
    c->timer_cb = &tcb->base;
    timer->start(timer);
}

/* ------------------------------------------------------------------------
 * Drain — nghttp3 → ngtcp2 → UDP send (GSO-coalesced)
 * ------------------------------------------------------------------------ */

void http3_connection_drain_out(http3_connection_t *c)
{
    if (c == NULL || c->closed || c->ngtcp2_conn == NULL) {
        return;
    }

    http3_packet_stats_t *stats = http3_listener_packet_stats(c->listener);

    /* Loop until ngtcp2 has nothing to emit. writev_stream returns the
     * number of bytes written into our buffer, 0 when congestion-
     * blocked, or a negative error code on terminal failure.
     *
     * GSO aggregation: pack N successive QUIC packets back-to-back into
     * `batch_buf`, then ship them with one sendmsg(+UDP_SEGMENT) syscall
     * (h2o lib/http3/common.c:208-223 pattern). Constraint: every
     * packet except possibly the last MUST equal `seg_size` bytes —
     * the kernel splits the buffer at exactly `seg_size` boundaries.
     * 64 slots × 1500 = 96 KiB. */
    enum { H3_GSO_BATCH_SLOTS = 64, H3_PKT_SLOT = 1500 };
    uint8_t batch_buf[H3_GSO_BATCH_SLOTS * H3_PKT_SLOT];
    size_t  batch_off   = 0;
    size_t  seg_size    = 0;
    size_t  batch_count = 0;
    /* GSO requires the same TOS byte for all packets in the batch (cmsg
     * is per-sendmsg, not per-segment). Track ECN for the current batch
     * and flush eagerly when ngtcp2 changes it. */
    uint8_t batch_ecn   = 0;

    /* Stable local sockaddr from listener bind config — must match what we
     * passed to ngtcp2_conn_server_new at accept time, or ngtcp2 rejects
     * the path. Peer address is pinned for the connection lifetime, so
     * both ends of the path are loop-invariant; build once, reuse for
     * every emitted packet. */
    struct sockaddr_storage local_addr;
    socklen_t local_addr_len = 0;
    http3_build_listener_local(c->listener, c->peer.ss_family,
                               &local_addr, &local_addr_len);
    ngtcp2_path_storage ps;
    ngtcp2_path_storage_init(&ps,
        (const struct sockaddr *)&local_addr, local_addr_len,
        (const struct sockaddr *)&c->peer,    c->peer_len,
        NULL);

    /* Per-call iteration cap. Defends against any pathological state
     * where ngtcp2/nghttp3 keep handing us bytes/no-bytes without ever
     * hitting break — observed only in theory (no fired bug), but the
     * loop is unbounded as written, so a misbehaving peer or library
     * regression could pin a worker thread. 4096 packets ≈ 5 MiB at
     * MTU 1280, far above any legitimate single-tick burst. */
    enum { H3_DRAIN_ITER_CAP = 4096 };
    unsigned drain_iter = 0;
    bool h3_framing_dead = false;  /* sticky once nghttp3 returns <0 */

#define H3_FLUSH_BATCH() do {                                                  \
        if (batch_off > 0) {                                                   \
            if (batch_count > 1) {                                             \
                (void)http3_listener_send_gso(c->listener,                     \
                    batch_buf, batch_off, seg_size, batch_ecn,                 \
                    (const struct sockaddr *)&c->peer, c->peer_len);           \
            } else {                                                           \
                (void)http3_listener_send_packet(c->listener,                  \
                    batch_buf, batch_off, batch_ecn,                           \
                    (const struct sockaddr *)&c->peer, c->peer_len);           \
            }                                                                  \
            if (stats != NULL) {                                               \
                stats->quic_packets_sent += batch_count;                       \
                stats->quic_bytes_sent   += (uint64_t)batch_off;               \
            }                                                                  \
            batch_off = 0; seg_size = 0; batch_count = 0; batch_ecn = 0;       \
        }                                                                      \
    } while (0)

    for (;;) {
        if (++drain_iter > H3_DRAIN_ITER_CAP) {
            if (stats != NULL) stats->quic_drain_iter_cap_hit++;
            break;
        }
        /* Need room for one max-size packet at the next slot. If batch
         * is near-full, flush first so writev_stream gets full slot. */
        if (batch_off + H3_PKT_SLOT > sizeof(batch_buf)) {
            H3_FLUSH_BATCH();
        }
        /* Ask nghttp3 for the next chunk of stream payload to send
         * (control-stream SETTINGS, encoder/decoder QPACK frames,
         * response HEADERS / DATA frames). The
         * iovec is then handed to ngtcp2_conn_writev_stream which packs
         * it into a QUIC STREAM frame. nghttp3 returns 0 when there is
         * nothing pending; we still call writev_stream below with
         * stream_id = -1 so ngtcp2 can emit ACK / control frames of its
         * own. */
        int64_t       h3_stream_id = -1;
        int           h3_fin       = 0;
        nghttp3_vec   h3_vec[16];
        nghttp3_ssize h3_veccnt = 0;

        if (c->nghttp3_conn != NULL && !h3_framing_dead) {
            h3_veccnt = nghttp3_conn_writev_stream(
                (nghttp3_conn *)c->nghttp3_conn,
                &h3_stream_id, &h3_fin,
                h3_vec, sizeof(h3_vec) / sizeof(h3_vec[0]));
            if (h3_veccnt < 0) {
                /* nghttp3 hit an unrecoverable framing error. Bump the
                 * counter once, latch the dead-flag so we don't loop
                 * back into it, and let ACK-only writev drain ngtcp2.
                 * Don't synthesize a CONNECTION_CLOSE here — emit_close()
                 * needs the listener and we're already deep in the drain
                 * pipeline; ngtcp2 idle-timeout will reap the half-broken
                 * connection on the next timer tick. RFC 9114 §8 wants
                 * H3_FRAME_ERROR; we record it via the counter. */
                if (stats != NULL) stats->h3_framing_error++;
                h3_framing_dead = true;
                h3_veccnt = 0;
                h3_stream_id = -1;
            }
        }

        ngtcp2_pkt_info pi = {0};
        ngtcp2_ssize pdatalen = 0;
        /* WRITE_MORE only meaningful when we actually have stream data;
         * passing it on a bare-handshake / ACK-only call asks ngtcp2 to
         * spin waiting for stream input that will never come. */
        uint32_t flags = (h3_stream_id >= 0)
            ? (NGTCP2_WRITE_STREAM_FLAG_MORE
               | (h3_fin ? NGTCP2_WRITE_STREAM_FLAG_FIN : 0))
            : 0;
        ngtcp2_ssize n = ngtcp2_conn_writev_stream(
            (ngtcp2_conn *)c->ngtcp2_conn,
            &ps.path, &pi,
            batch_buf + batch_off, H3_PKT_SLOT,
            &pdatalen,
            flags,
            h3_stream_id,
            (const ngtcp2_vec *)h3_vec, (size_t)h3_veccnt,
            http3_ts_now());

        if (n == NGTCP2_ERR_WRITE_MORE) {
            /* WRITE_MORE means ngtcp2 accepted pdatalen bytes from
             * nghttp3 into the next packet but has room for more. Tell
             * nghttp3 the bytes are committed and keep going — without
             * resetting the iovec, so the next writev sees the
             * remainder. */
            if (c->nghttp3_conn != NULL && pdatalen > 0) {
                nghttp3_conn_add_write_offset(
                    (nghttp3_conn *)c->nghttp3_conn,
                    h3_stream_id, (size_t)pdatalen);
            }
            continue;
        }
        if (n == NGTCP2_ERR_STREAM_DATA_BLOCKED || n == NGTCP2_ERR_STREAM_SHUT_WR) {
            /* Flow-control or half-closed write side. Pause the stream
             * so nghttp3 stops handing us data on it until ngtcp2
             * extends the window via extend_max_stream_data callback. */
            if (c->nghttp3_conn != NULL) {
                nghttp3_conn_block_stream(
                    (nghttp3_conn *)c->nghttp3_conn, h3_stream_id);
            }
            continue;
        }
        if (n == 0) {
            /* No outgoing datagram produced. If nghttp3 had data ready
             * and ngtcp2 still produced nothing, ack the bytes anyway
             * (avoids a spin loop). Otherwise we're truly idle. */
            if (c->nghttp3_conn != NULL && pdatalen > 0) {
                nghttp3_conn_add_write_offset(
                    (nghttp3_conn *)c->nghttp3_conn,
                    h3_stream_id, (size_t)pdatalen);
                continue;
            }
            H3_FLUSH_BATCH();
            break;
        }
        if (n < 0) {
            if (stats != NULL) stats->quic_write_error++;
            H3_FLUSH_BATCH();
            break;
        }
        if (c->nghttp3_conn != NULL && pdatalen > 0) {
            nghttp3_conn_add_write_offset(
                (nghttp3_conn *)c->nghttp3_conn,
                h3_stream_id, (size_t)pdatalen);
        }

        /* Append the freshly-written packet to the batch. GSO requires
         * all but the last segment in a batch to be exactly seg_size
         * bytes; we enforce that here by flushing eagerly when the
         * size pattern breaks. ECN must also match across the batch
         * (cmsg(IP_TOS) is per-sendmsg) — flush eagerly if it changes. */
        size_t pkt_len = (size_t)n;
        uint8_t pkt_ecn = pi.ecn;
        if (batch_count == 0) {
            seg_size  = pkt_len;
            batch_off = pkt_len;
            batch_count = 1;
            batch_ecn = pkt_ecn;
        } else if (pkt_len == seg_size && pkt_ecn == batch_ecn) {
            batch_off += pkt_len;
            batch_count++;
        } else if (pkt_len < seg_size && pkt_ecn == batch_ecn) {
            /* Natural last packet of a flight (typically a short
             * trailer). Append + flush so the kernel sees all earlier
             * segments at full seg_size and this one as the final
             * fragment. */
            batch_off += pkt_len;
            batch_count++;
            H3_FLUSH_BATCH();
        } else {
            /* Mismatch (longer packet, or ECN changed). Flush the previous
             * batch (without this packet) then memmove the just-written
             * packet to the start and seed a new batch. */
            uint8_t tmp[H3_PKT_SLOT];
            memcpy(tmp, batch_buf + batch_off, pkt_len);
            H3_FLUSH_BATCH();
            memcpy(batch_buf, tmp, pkt_len);
            batch_off = pkt_len;
            seg_size  = pkt_len;
            batch_count = 1;
            batch_ecn = pkt_ecn;
        }
    }
    /* Drain-loop exit (cap, error, or no more packets) — flush whatever
     * is left in the batch. */
    H3_FLUSH_BATCH();
#undef H3_FLUSH_BATCH
}

/* ------------------------------------------------------------------------
 * Connection close — graceful CONNECTION_CLOSE / reap / terminal probe
 * ------------------------------------------------------------------------ */

/* Synchronously emit one CONNECTION_CLOSE datagram for this
 * connection. Safe to call only when ngtcp2 has at least one packet
 * worth of state (post-server_new). Sets sent_connection_close so the
 * teardown path does not duplicate the emit.
 *
 * We use the application-error variant with NGHTTP3_H3_NO_ERROR (0x100)
 * once nghttp3 is alive — that is the polite "I am closing this HTTP/3
 * connection cleanly" code per RFC 9114 §8. Pre-handshake (no nghttp3
 * yet) we emit a transport NO_ERROR. ngtcp2 caches the produced packet
 * so subsequent write_connection_close calls return the same bytes —
 * we currently call this once and reap, accepting the spec-recommended
 * 3*PTO retention as a future polish (peer retransmit during the window
 * goes unrouted but the kernel + their PTO sort it out). */
void http3_connection_emit_close(http3_connection_t *c)
{
    if (c == NULL || c->sent_connection_close || c->ngtcp2_conn == NULL) {
        return;
    }
    ngtcp2_conn *qc = (ngtcp2_conn *)c->ngtcp2_conn;

    /* Already in draining: peer told us. Per ngtcp2 docs we MUST NOT emit
     * a CONNECTION_CLOSE in that state. Just mark and return. */
    if (ngtcp2_conn_in_draining_period(qc)) {
        c->sent_connection_close = true;
        return;
    }

    ngtcp2_ccerr ccerr;
    if (c->nghttp3_conn != NULL) {
        ngtcp2_ccerr_set_application_error(
            &ccerr, NGHTTP3_H3_NO_ERROR, NULL, 0);
    } else {
        ngtcp2_ccerr_default(&ccerr);
    }

    struct sockaddr_storage local_addr;
    socklen_t local_addr_len = 0;
    http3_build_listener_local(c->listener, c->peer.ss_family,
                               &local_addr, &local_addr_len);
    ngtcp2_path_storage ps = {0};
    ngtcp2_path_storage_init(&ps,
        (const struct sockaddr *)&local_addr, local_addr_len,
        (const struct sockaddr *)&c->peer,    c->peer_len,
        NULL);
    ngtcp2_pkt_info pi = {0};

    uint8_t buf[1280];
    ngtcp2_ssize n = ngtcp2_conn_write_connection_close(
        qc, &ps.path, &pi, buf, sizeof(buf), &ccerr, http3_ts_now());
    /* Flag emission unconditionally — n<=0 means ngtcp2 had no packet to
     * produce (pre-handshake / already-in-closing-without-buffered-pkt),
     * which is fine: we still want to skip the second emit on teardown. */
    c->sent_connection_close = true;
    if (n <= 0) {
        return;
    }

    (void)http3_listener_send_packet(c->listener, buf, (size_t)n, pi.ecn,
        (const struct sockaddr *)&c->peer, c->peer_len);
    http3_packet_stats_t *stats = http3_listener_packet_stats(c->listener);
    if (stats != NULL) {
        stats->quic_connection_close_sent++;
        stats->quic_packets_sent++;
        stats->quic_bytes_sent += (uint64_t)n;
    }
}

/* Public reap: unhook from listener (list + map) then free. The free
 * path emits a graceful CONNECTION_CLOSE if we have not already and we
 * are not in the draining period. */
void http3_connection_reap(http3_connection_t *conn)
{
    if (conn == NULL || conn->closed) {
        return;
    }
    http3_packet_stats_t *stats = http3_listener_packet_stats(conn->listener);
    if (stats != NULL) {
        stats->quic_conn_reaped++;
    }
    http3_listener_remove_connection(conn->listener, conn);
    http3_connection_free(conn);
}

/* Post-IO terminal probe. Called after read_pkt and after handle_expiry
 * so ngtcp2 has had a chance to settle into closing/draining/idle.
 * Returns true iff the caller must NOT touch `c` again — it has been
 * reaped and freed. */
bool http3_connection_check_terminal(http3_connection_t *c)
{
    if (c == NULL || c->closed || c->ngtcp2_conn == NULL) {
        return c == NULL || c->closed;
    }
    ngtcp2_conn *qc = (ngtcp2_conn *)c->ngtcp2_conn;
    http3_packet_stats_t *stats = http3_listener_packet_stats(c->listener);
    bool draining = ngtcp2_conn_in_draining_period(qc);
    bool closing  = ngtcp2_conn_in_closing_period(qc);
    if (!draining && !closing) {
        return false;
    }
    if (stats != NULL) {
        if (closing)  stats->quic_conn_in_closing++;
        if (draining) stats->quic_conn_in_draining++;
    }
    /* Closing period: re-emit the close packet so retransmits are
     * answered. Draining: peer initiated; emit_close is a no-op there. */
    http3_connection_emit_close(c);
    http3_connection_reap(c);
    return true;
}

