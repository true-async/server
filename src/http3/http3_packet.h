#ifndef HTTP3_PACKET_H
#define HTTP3_PACKET_H

#include <zend.h>
#include <zend_async_API.h>
#include <stdint.h>
#include <stddef.h>
#ifdef PHP_WIN32
# include <winsock2.h>
# include <ws2tcpip.h>
#else
# include <sys/socket.h>
#endif

/* Per-listener wire-level counters. Populated by the dispatch path in
 * http3_connection.c — the split keeps the stat schema in one header
 * that both TUs can include. */
typedef struct _http3_packet_stats_s {
    uint64_t quic_initial;            /* long header, supported version */
    uint64_t quic_short_header;       /* 1-RTT datagram (post-handshake) */
    uint64_t quic_version_negotiated; /* VERSION_NEGOTIATION responses sent */
    uint64_t quic_parse_errors;       /* NGTCP2_ERR_INVALID_ARGUMENT from pkt_decode */
    uint64_t quic_conn_accepted;      /* ngtcp2_conn_server_new succeeded */
    uint64_t quic_conn_rejected;      /* ngtcp2_conn_server_new failed */

    /* Read-loop counters. */
    uint64_t quic_read_ok;            /* ngtcp2_conn_read_pkt returned 0 */
    uint64_t quic_read_error;         /* returned a non-fatal error */
    uint64_t quic_read_fatal;         /* returned a fatal error (conn dead) */

    /* Write-loop / timer counters. */
    uint64_t quic_packets_sent;       /* datagrams emitted by writev_stream */
    uint64_t quic_bytes_sent;         /* cumulative bytes over those datagrams */
    uint64_t quic_timer_fired;        /* handle_expiry invocations */
    uint64_t quic_write_error;        /* writev_stream non-zero / negative */

    /* Handshake / ALPN counters. */
    uint64_t quic_handshake_completed; /* TLS 1.3 handshake reached RFC 9001 done */
    uint64_t quic_alpn_mismatch;       /* peer negotiated something other than "h3" */

    /* nghttp3 lifecycle counters. */
    uint64_t h3_init_ok;               /* nghttp3_conn allocated + uni-streams bound */
    uint64_t h3_init_failed;           /* alloc / bind failed; conn closed */
    uint64_t h3_stream_close;          /* nghttp3_conn_close_stream invocations */
    uint64_t h3_stream_read_error;     /* nghttp3_conn_read_stream returned <0 */

    /* Request-assembly counters. */
    uint64_t h3_request_received;      /* end_stream reached on a request stream */
    uint64_t h3_request_oversized;     /* peer exceeded our headers/body caps */
    uint64_t h3_streams_opened;        /* begin_headers fired for a new request stream */

    /* Response counters. */
    uint64_t h3_response_submitted;    /* nghttp3_conn_submit_response succeeded */
    uint64_t h3_response_submit_error; /* submit_response returned non-zero */

    /* Connection lifecycle / close counters. */
    uint64_t quic_connection_close_sent; /* CONNECTION_CLOSE datagram emitted */
    uint64_t quic_conn_in_closing;       /* observed in_closing_period after IO */
    uint64_t quic_conn_in_draining;      /* observed in_draining_period after IO */
    uint64_t quic_conn_idle_closed;      /* handle_expiry returned NGTCP2_ERR_IDLE_CLOSE */
    uint64_t quic_conn_handshake_timeout;/* handle_expiry returned NGTCP2_ERR_HANDSHAKE_TIMEOUT */
    uint64_t quic_conn_reaped;           /* connections reaped via terminal-state path */

    /* Stateless reset (RFC 9000 §10.3). */
    uint64_t quic_stateless_reset_sent;  /* stateless-reset packets emitted */

    /* Retry / address validation (RFC 9000 §8.1.2). */
    uint64_t quic_retry_sent;            /* Retry packets emitted (no token in Initial) */
    uint64_t quic_retry_token_ok;        /* Initials accepted after token verify */
    uint64_t quic_retry_token_invalid;   /* Initials dropped: bad/expired/forged token */

    /* Per-peer connection budget. */
    uint64_t quic_conn_per_peer_rejected;

    /* Audit hardening counters. */
    uint64_t h3_framing_error;        /* nghttp3_conn_writev_stream returned <0 */
    uint64_t quic_drain_iter_cap_hit; /* drain_out hit per-call iteration cap */

    /* Send-path error categorisation. Every outbound sendmsg lands in
     * exactly one of these buckets (success increments quic_packets_sent
     * already accounted above). Use to detect kernel-side trouble that
     * would otherwise be invisible — GSO refusal, MTU shrink, peer-port
     * gone. */
    uint64_t quic_send_eagain;        /* kernel UDP buffer full — packet dropped, QUIC retransmits */
    uint64_t quic_send_gso_refused;   /* sendmsg(UDP_SEGMENT) returned EIO/EINVAL — fell back to per-segment */
    uint64_t quic_send_emsgsize;      /* packet exceeded path MTU with DF set — ngtcp2 PMTU should react */
    uint64_t quic_send_unreach;       /* EHOSTUNREACH / ENETUNREACH — peer or route gone */
    uint64_t quic_send_other_error;   /* anything not in the categories above */
    uint64_t quic_gso_disabled;       /* set to 1 once we permanently turn GSO off after first refusal */

    /* Async errors retrieved via MSG_ERRQUEUE. These come from ICMP
     * responses and delayed kernel send failures — they tell us what
     * happened to packets sendmsg() already accepted. */
    uint64_t quic_errqueue_emsgsize;  /* ICMP "frag needed" with DF set — path MTU is smaller than we assumed */
    uint64_t quic_errqueue_unreach;   /* ICMP destination/port unreachable */
    uint64_t quic_errqueue_other;     /* other extended-error origins (zerocopy, txtime, …) */
} http3_packet_stats_t;

/* Write + send a QUIC Version Negotiation datagram to `peer` over the
 * listener's socket. Returns true on success.
 *
 * Exposed as a helper because both the no-connection fast path (in
 * dispatch) and — eventually — ngtcp2's version_negotiation callback may
 * want to emit one. */
struct _ngtcp2_version_cid_s;
typedef struct _http3_listener_s http3_listener_t;  /* defined in http3_listener.h */
bool http3_packet_send_version_negotiation(
    http3_listener_t *listener,
    const uint8_t *dcid, size_t dcidlen,
    const uint8_t *scid, size_t scidlen,
    const struct sockaddr *peer, socklen_t peer_len);

/* Derive a stateless-reset token from the listener-static
 * 32-byte key and the destination CID. Token is HMAC-SHA256(key,
 * dcid)[0:16]. Deterministic so retransmits of the same dead-conn
 * datagram produce the same token, and so it matches what we issued
 * via NEW_CONNECTION_ID at handshake time (clients verify the token
 * against the cached value). out must point to 16 bytes. */
void http3_packet_compute_sr_token(const uint8_t key[32],
                                   const uint8_t *dcid, size_t dcidlen,
                                   uint8_t out[16]);

/* Emit a stateless-reset datagram as response to a 1-RTT
 * packet whose DCID does not match any live connection. The wire
 * looks like a random 1-RTT packet but the trailing 16 bytes carry
 * the SR token so the original peer can recognise + abandon the
 * dead connection. Caller is responsible for the spec-mandated rate
 * limiting (don't reply to a smaller packet, don't reply faster
 * than the inbound rate). Returns true iff a packet was sent. */
bool http3_packet_send_stateless_reset(
    http3_listener_t *listener,
    const uint8_t key[32],
    const uint8_t *dcid, size_t dcidlen,
    size_t inbound_datagram_len,
    const struct sockaddr *peer, socklen_t peer_len);

/* Emit a Retry packet (RFC 9000 §8.1.2). Used to validate the peer's
 * source address before allocating an ngtcp2_conn — bounds the
 * amplification gap an attacker with a spoofed source IP could exploit.
 * `client_dcid` is the DCID from the client's Initial (becomes our
 * Original DCID), `client_scid` becomes the new packet's DCID. Returns
 * true iff the Retry was generated and submitted to the socket. */
bool http3_packet_send_retry(
    http3_listener_t *listener,
    const uint8_t retry_token_key[32],
    uint32_t version,
    const uint8_t *client_dcid, size_t client_dcid_len,
    const uint8_t *client_scid, size_t client_scid_len,
    const struct sockaddr *peer, socklen_t peer_len);

/* Verify a Retry token carried in a client Initial. On success returns
 * 0 and writes the original DCID (the DCID from the Initial that
 * triggered the Retry) into *odcid_out. Returns -1 on failure: bad
 * format, bad MAC, expired, or address mismatch. */
int http3_packet_verify_retry_token(
    const uint8_t retry_token_key[32],
    uint32_t version,
    const uint8_t *token, size_t tokenlen,
    const uint8_t *current_dcid, size_t current_dcid_len,
    const struct sockaddr *peer, socklen_t peer_len,
    uint8_t *odcid_out, size_t *odcid_len_out);

#endif /* HTTP3_PACKET_H */
