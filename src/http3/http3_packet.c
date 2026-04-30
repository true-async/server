#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <php.h>
#include "Zend/zend_hrtime.h"
#include "http3_packet.h"
#include "http3_listener.h"


#include <ngtcp2/ngtcp2.h>
#include <ngtcp2/ngtcp2_crypto.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <string.h>

/* Retry token validity window. Long enough that a normal RTT + handshake
 * round-trip never expires it; short enough that captured tokens age
 * out before they can be replayed. */
#define HTTP3_RETRY_TOKEN_TIMEOUT_NS (10ull * 1000ull * 1000ull * 1000ull)

/* Reserved greasing version per RFC 9000 §15. A stable value keeps our
 * output bit-for-bit reproducible; grease is meant to exercise the
 * client's version-negotiation logic, not to randomise our wire bytes. */
#define HTTP3_GREASE_VERSION 0x1a2a3a4aU

bool http3_packet_send_version_negotiation(
    http3_listener_t *listener,
    const uint8_t *dcid, size_t dcidlen,
    const uint8_t *scid, size_t scidlen,
    const struct sockaddr *peer, socklen_t peer_len)
{
    if (UNEXPECTED(listener == NULL || peer == NULL)) {
        return false;
    }

    /* Advertise [GREASE, V1]. The grease entry forces conforming clients
     * to ignore unknown versions cleanly — exercises the spec path. */
    uint32_t sv[] = { HTTP3_GREASE_VERSION, NGTCP2_PROTO_VER_V1 };

    uint8_t buf[1280]; /* QUIC minimum MTU — ample for VN. */

    ngtcp2_ssize n = ngtcp2_pkt_write_version_negotiation(
        buf, sizeof(buf),
        /* unused_random: */ 0,
        dcid, dcidlen, scid, scidlen,
        sv, sizeof(sv) / sizeof(sv[0]));

    if (UNEXPECTED(n < 0)) {
        return false;
    }

    /* Fire-and-forget — a stateless VN response the client will retry
     * anyway. ECN = 0 (stateless reply, no cong-control bookkeeping). */
    (void)http3_listener_send_packet(listener, buf, (size_t)n, 0, peer, peer_len);
    return true;
}

void http3_packet_compute_sr_token(const uint8_t key[32],
                                   const uint8_t *dcid, size_t dcidlen,
                                   uint8_t out[16])
{
    uint8_t mac[32];
    unsigned int maclen = sizeof(mac);
    if (HMAC(EVP_sha256(), key, 32, dcid, dcidlen, mac, &maclen) == NULL
        || maclen < 16) {
        /* Should not happen with EVP_sha256 + 32-byte key; zero-fill
         * the output so callers don't accidentally publish stack data
         * if HMAC ever did fail. */
        memset(out, 0, 16);
        return;
    }
    memcpy(out, mac, 16);
}

bool http3_packet_send_stateless_reset(
    http3_listener_t *listener,
    const uint8_t key[32],
    const uint8_t *dcid, size_t dcidlen,
    size_t inbound_datagram_len,
    const struct sockaddr *peer, socklen_t peer_len)
{
    if (listener == NULL || peer == NULL || dcid == NULL || dcidlen == 0) {
        return false;
    }
    /* RFC 9000 §10.3: server MUST NOT send a stateless reset that is
     * three times larger than the packet that triggered it (anti-
     * amplification), and the packet itself must be at least 22 bytes
     * to be indistinguishable from a 1-RTT packet. We pick a size
     * just under min(inbound-1, 1200) but at least 22, so it cannot
     * loop into an amplification spiral if the trigger was a tiny
     * forged packet. */
    if (inbound_datagram_len < 41) {
        /* RFC 9000 §10.3 — minimum trigger size: a stateless reset is
         * itself at least 21 bytes; replying to a smaller packet would
         * exceed the 1×-amplification cap. */
        return false;
    }

    size_t reply_len = inbound_datagram_len - 1;
    if (reply_len > 1200) reply_len = 1200;
    if (reply_len < 22)   reply_len = 22;

    uint8_t buf[1200];
    /* Fill with random bytes so the packet looks like an encrypted
     * 1-RTT payload to anyone other than the original peer. */
    if (RAND_bytes(buf, (int)reply_len) != 1) {
        return false;
    }
    /* First byte must have form `01xxxxxx` (header form bit 0, fixed
     * bit 1) to be a valid short header. RAND_bytes already gave us
     * a random byte; force the two header bits. */
    buf[0] = (uint8_t)((buf[0] & 0x3f) | 0x40);

    /* Trailing 16 bytes = SR token derived from the inbound DCID. */
    uint8_t token[16];
    http3_packet_compute_sr_token(key, dcid, dcidlen, token);
    memcpy(buf + reply_len - 16, token, 16);

    (void)http3_listener_send_packet(listener, buf, reply_len, 0, peer, peer_len);
    return true;
}

bool http3_packet_send_retry(
    http3_listener_t *listener,
    const uint8_t retry_token_key[32],
    uint32_t version,
    const uint8_t *client_dcid, size_t client_dcid_len,
    const uint8_t *client_scid, size_t client_scid_len,
    const struct sockaddr *peer, socklen_t peer_len)
{
    if (listener == NULL || peer == NULL || retry_token_key == NULL
        || client_dcid == NULL || client_dcid_len == 0) {
        return false;
    }

    /* Server-chosen Source CID for the Retry. Random 18 bytes — same
     * length as our HTTP3_SCID_LEN. */
    ngtcp2_cid retry_scid;
    retry_scid.datalen = 18;
    if (RAND_bytes(retry_scid.data, (int)retry_scid.datalen) != 1) {
        return false;
    }

    ngtcp2_cid odcid;
    odcid.datalen = client_dcid_len;
    memcpy(odcid.data, client_dcid, client_dcid_len);

    uint8_t token[NGTCP2_CRYPTO_MAX_RETRY_TOKENLEN];
    ngtcp2_tstamp ts = (ngtcp2_tstamp)zend_hrtime();
    ngtcp2_ssize tokenlen = ngtcp2_crypto_generate_retry_token(
        token, retry_token_key, 32, version,
        (const ngtcp2_sockaddr *)peer, (ngtcp2_socklen)peer_len,
        &retry_scid, &odcid, ts);
    if (tokenlen < 0) {
        return false;
    }

    /* Retry packet's DCID is the client's SCID (echoed back). */
    ngtcp2_cid pkt_dcid;
    pkt_dcid.datalen = client_scid_len;
    if (client_scid_len > 0) {
        memcpy(pkt_dcid.data, client_scid, client_scid_len);
    }

    uint8_t buf[1280];
    ngtcp2_ssize n = ngtcp2_crypto_write_retry(
        buf, sizeof(buf), version,
        &pkt_dcid, &retry_scid, &odcid, token, (size_t)tokenlen);
    if (n < 0) {
        return false;
    }

    return http3_listener_send_packet(
        listener, buf, (size_t)n, 0, peer, peer_len);
}

int http3_packet_verify_retry_token(
    const uint8_t retry_token_key[32],
    uint32_t version,
    const uint8_t *token, size_t tokenlen,
    const uint8_t *current_dcid, size_t current_dcid_len,
    const struct sockaddr *peer, socklen_t peer_len,
    uint8_t *odcid_out, size_t *odcid_len_out)
{
    if (retry_token_key == NULL || token == NULL || tokenlen == 0
        || current_dcid == NULL || peer == NULL
        || odcid_out == NULL || odcid_len_out == NULL) {
        return -1;
    }

    ngtcp2_cid dcid;
    dcid.datalen = current_dcid_len;
    if (current_dcid_len > 0) {
        memcpy(dcid.data, current_dcid, current_dcid_len);
    }

    ngtcp2_cid odcid;
    ngtcp2_tstamp ts = (ngtcp2_tstamp)zend_hrtime();
    int rv = ngtcp2_crypto_verify_retry_token(
        &odcid, token, tokenlen,
        retry_token_key, 32, version,
        (const ngtcp2_sockaddr *)peer, (ngtcp2_socklen)peer_len,
        &dcid, HTTP3_RETRY_TOKEN_TIMEOUT_NS, ts);
    if (rv != 0) {
        return -1;
    }

    if (odcid.datalen > NGTCP2_MAX_CIDLEN) {
        return -1;
    }
    memcpy(odcid_out, odcid.data, odcid.datalen);
    *odcid_len_out = odcid.datalen;
    return 0;
}

