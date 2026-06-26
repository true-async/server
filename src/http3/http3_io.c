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
#include "http3/http3_stream.h"   /* hq egress sources from http3_stream_t */

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

        /* Per-connection ACK/PTO service delay (#80 Phase 0): how far past
         * its armed deadline this timer actually fired. Anything beyond the
         * reactor budget means the reactor was busy when this connection's
         * ACK/loss timer was due. */
        if (c->timer_expiry_ns != 0) {
            const uint64_t now_ns = (uint64_t)http3_ts_now();

            if (now_ns > c->timer_expiry_ns) {
                const uint64_t late = now_ns - c->timer_expiry_ns;

                if (late > stats->reactor_max_timer_late_ns) {
                    stats->reactor_max_timer_late_ns = late;
                }

                if (late > http3_reactor_budget_ns()) {
                    stats->reactor_timer_late++;
                }
            }
        }
    }

    c->timer_expiry_ns = 0;

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
    if (c == NULL || c->closed || c->ngtcp2_conn == NULL) {
        return;
    }

    ngtcp2_tstamp expiry = ngtcp2_conn_get_expiry((ngtcp2_conn *)c->ngtcp2_conn);

    if (expiry == UINT64_MAX) {
        /* ngtcp2 has nothing scheduled — drop any stale timer. */
        c->timer_expiry_ns = 0;
        http3_connection_detach_timer(c);
        return;
    }

    /* Stamp the deadline we're arming for so timer_fire_cb can measure how
     * late the fire actually was (reactor service delay, #80 Phase 0). */
    c->timer_expiry_ns = (uint64_t)expiry;

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
    /* Per-batch send destination: ngtcp2 fills ps.path per packet (usually
     * c->peer; a migration probe can target a fresh path), so GSO must only
     * coalesce same-destination packets. */
    struct sockaddr_storage batch_peer;
    socklen_t batch_peer_len = 0;

    /* Stable local sockaddr from listener bind config — must match what we
     * passed to ngtcp2_conn_server_new at accept time, or ngtcp2 rejects
     * the path. */
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
                    (const struct sockaddr *)&batch_peer, batch_peer_len);     \
            } else {                                                           \
                (void)http3_listener_send_packet(c->listener,                  \
                    batch_buf, batch_off, batch_ecn,                           \
                    (const struct sockaddr *)&batch_peer, batch_peer_len);     \
            }                                                                  \
            if (stats != NULL) {                                               \
                stats->quic_packets_sent += batch_count;                       \
                stats->quic_bytes_sent   += (uint64_t)batch_off;               \
            }                                                                  \
            batch_off = 0; seg_size = 0; batch_count = 0; batch_ecn = 0;       \
        }                                                                      \
    } while (0)

    /* Phase 2 — opt-in pacing (#59, HttpServerConfig::setHttp3Pacing). OFF
     * by default: the block below is inert and the drain runs exactly as
     * before. ON: cap each burst at the congestion controller's
     * send_quantum and yield to the timer only for a real inter-burst gap
     * (> H3_PACING_MIN_DELAY_NS) so a lossless path still drains inline. */
    const bool pacing = (c->view != NULL && c->view->http3_pacing);
    enum { H3_PACING_MIN_DELAY_NS = 1000000 };   /* 1 ms */
    size_t   send_quantum  = pacing
        ? ngtcp2_conn_get_send_quantum((ngtcp2_conn *)c->ngtcp2_conn) : 0;
    size_t   quantum_sent  = 0;
    uint64_t drained_bytes = 0;
    bool     paced_yield   = false;

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
        int64_t        h3_stream_id = -1;
        int            h3_fin       = 0;
        nghttp3_vec    h3_vec[16];
        nghttp3_ssize  h3_veccnt = 0;
        http3_stream_t *hq_cur   = NULL;

        if (c->proto == HTTP3_PROTO_HQ) {
            /* hq-interop: source raw response bytes from a served stream's
             * hq_body (mmap'd file or a literal) — no nghttp3 framing. Pick the
             * first stream whose response is ready and whose FIN has not gone
             * out yet; the FIN rides the tail of the body (or a bare FIN when
             * the body is empty). */
            for (http3_stream_t *s = c->streams_head; s != NULL; s = s->list_next) {
                if (s->hq_served && !s->hq_fin_sent) {
                    hq_cur = s;
                    break;
                }
            }

            if (hq_cur != NULL) {
                const size_t remain = hq_cur->hq_body_len - hq_cur->hq_body_off;

                h3_stream_id = hq_cur->stream_id;
                if (remain > 0) {
                    h3_vec[0].base = (uint8_t *)hq_cur->hq_body + hq_cur->hq_body_off;
                    h3_vec[0].len  = remain;
                    h3_veccnt      = 1;
                } else {
                    h3_veccnt      = 0;   /* empty body / fully drained: bare FIN */
                }
                h3_fin = 1;
            }
        } else if (c->nghttp3_conn != NULL && !h3_framing_dead) {
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
         * spin waiting for stream input that will never come. hq omits
         * WRITE_MORE: with no nghttp3 coalescing there is no remainder to
         * resume, and MORE on a bare FIN would loop forever. */
        uint32_t flags = 0;

        if (h3_stream_id >= 0) {
            flags = h3_fin ? NGTCP2_WRITE_STREAM_FLAG_FIN : 0;

            if (c->proto != HTTP3_PROTO_HQ) {
                flags |= NGTCP2_WRITE_STREAM_FLAG_MORE;
            }
        }
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
            } else if (hq_cur != NULL && pdatalen > 0) {
                hq_cur->hq_body_off += (size_t)pdatalen;
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
                continue;
            }

            /* hq has no per-stream block list; end this drain and let a later
             * tick resume once the window opens (proper cwnd-wake arrives with
             * the large-file hq path). */
            H3_FLUSH_BATCH();
            break;
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
            } else if (hq_cur != NULL && pdatalen > 0) {
                hq_cur->hq_body_off += (size_t)pdatalen;
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
        } else if (hq_cur != NULL) {
            /* pdatalen is -1 when the packet carried no stream data (ACK/control
             * only) — must NOT advance then, or (size_t)(-1) wraps the offset
             * backward and the next pick re-sends bytes. Mirrors the h3 guard. */
            if (pdatalen > 0) {
                hq_cur->hq_body_off += (size_t)pdatalen;
            }

            /* FLAG_FIN rode the final data packet; once the body is fully
             * drained the FIN is out, so stop re-picking the stream (it stays
             * alive until stream_close releases the slab). Covers the empty
             * body bare-FIN (off == len == 0). */
            if (hq_cur->hq_body_off >= hq_cur->hq_body_len) {
                hq_cur->hq_fin_sent = true;
            }
        }

        /* Append the freshly-written packet to the batch. GSO requires
         * all but the last segment in a batch to be exactly seg_size
         * bytes; we enforce that here by flushing eagerly when the
         * size pattern breaks. ECN must also match across the batch
         * (cmsg(IP_TOS) is per-sendmsg) — flush eagerly if it changes. */
        size_t pkt_len = (size_t)n;
        uint8_t pkt_ecn = pi.ecn;
        /* ngtcp2 reported the destination for this packet in ps.path — copy
         * it out before the next writev overwrites the storage. Usually ==
         * c->peer; differs for a migration probe on a fresh path. */
        const struct sockaddr *pkt_peer = ps.path.remote.addr;
        const socklen_t pkt_peer_len = (socklen_t)ps.path.remote.addrlen;
        const bool same_peer = batch_count > 0
            && batch_peer_len == pkt_peer_len
            && memcmp(&batch_peer, pkt_peer, (size_t)pkt_peer_len) == 0;

        if (batch_count == 0) {
            seg_size  = pkt_len;
            batch_off = pkt_len;
            batch_count = 1;
            batch_ecn = pkt_ecn;
            memcpy(&batch_peer, pkt_peer, (size_t)pkt_peer_len);
            batch_peer_len = pkt_peer_len;
        } else if (pkt_len == seg_size && pkt_ecn == batch_ecn && same_peer) {
            batch_off += pkt_len;
            batch_count++;
        } else if (pkt_len < seg_size && pkt_ecn == batch_ecn && same_peer) {
            /* Natural last packet of a flight (typically a short
             * trailer). Append + flush so the kernel sees all earlier
             * segments at full seg_size and this one as the final
             * fragment. */
            batch_off += pkt_len;
            batch_count++;
            H3_FLUSH_BATCH();
        } else {
            /* Mismatch (longer packet, ECN changed, or a different
             * destination path — e.g. a migration probe). Flush the previous
             * batch (without this packet) then memmove the just-written
             * packet to the start and seed a new batch on its own dest. */
            uint8_t tmp[H3_PKT_SLOT];
            memcpy(tmp, batch_buf + batch_off, pkt_len);
            H3_FLUSH_BATCH();
            memcpy(batch_buf, tmp, pkt_len);
            batch_off = pkt_len;
            seg_size  = pkt_len;
            batch_count = 1;
            batch_ecn = pkt_ecn;
            memcpy(&batch_peer, pkt_peer, (size_t)pkt_peer_len);
            batch_peer_len = pkt_peer_len;
        }

        if (pacing) {
            drained_bytes += pkt_len;
            quantum_sent  += pkt_len;

            if (send_quantum > 0 && quantum_sent >= send_quantum) {
                /* Quantum spent. Set the next pacing tx-time (ngtcp2 counts
                 * the bytes it handed us via writev, regardless of our GSO
                 * buffering), then read whether it wants a real gap. */
                ngtcp2_conn_update_pkt_tx_time((ngtcp2_conn *)c->ngtcp2_conn,
                                               http3_ts_now());
                const ngtcp2_tstamp now_ts = http3_ts_now();
                const ngtcp2_tstamp expiry =
                    ngtcp2_conn_get_expiry((ngtcp2_conn *)c->ngtcp2_conn);

                if (expiry != UINT64_MAX
                    && expiry > now_ts + (ngtcp2_tstamp)H3_PACING_MIN_DELAY_NS) {
                    /* Real inter-burst gap — ship what's batched, yield. */
                    H3_FLUSH_BATCH();
                    paced_yield = true;
                    break;
                }

                /* Sub-threshold gap (loopback / fast path) — keep draining
                 * inline; no flush, so GSO aggregation is preserved. */
                quantum_sent = 0;
                send_quantum = ngtcp2_conn_get_send_quantum((ngtcp2_conn *)c->ngtcp2_conn);
            }
        }
    }
    /* Drain-loop exit (cap, error, paced yield, or no more packets) — flush
     * whatever is left in the batch. */
    H3_FLUSH_BATCH();

    /* Pacing tail: keep ngtcp2's pacing state current; on a paced yield arm
     * the timer ourselves so the remainder reschedules regardless of caller.
     * Both no-op when pacing is off. */
    if (pacing && drained_bytes > 0) {
        ngtcp2_conn_update_pkt_tx_time((ngtcp2_conn *)c->ngtcp2_conn,
                                       http3_ts_now());
    }

    if (paced_yield) {
        http3_connection_arm_timer(c);
    }
#undef H3_FLUSH_BATCH
}

/* Flush one connection and settle its lifecycle. This is the read path's
 * post-drain tail, lifted out so the deferred dirty-list flush and the
 * per-datagram path run identical logic: drain, then reap-or-arm. On a
 * terminal transition check_terminal frees the conn and returns true, so
 * the timer is armed only on the live branch and `c` is dead afterward. */
void http3_connection_flush(http3_connection_t *c)
{
    /* Migration storm flagged on read: shed the connection now. emit_close
     * targets c->peer — the live (migrated) address — so the close reaches the
     * client, which reconnects cleanly instead of stalling on a wedged path. */
    if (c->migration_storm) {
        http3_packet_stats_t *stats = http3_listener_packet_stats(c->listener);

        if (stats != NULL) {
            stats->quic_migration_storm_shed++;
        }

        http3_connection_emit_close(c);
        http3_connection_reap(c);
        return;
    }

    http3_connection_drain_out(c);

    if (!http3_connection_check_terminal(c)) {
        http3_connection_arm_timer(c);
    }
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

