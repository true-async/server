/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+

  HTTP/3 connection lifecycle (accept / packet entry / free) plus the
  small process-lifetime helpers (timestamp, RNG, OSSL crypto init,
  ngtcp2 debug-logger bridge, listener local-sockaddr fabrication).

  This is the trimmed shell that remains after the audit-#8 split:
    - http3_io.c          : timer + drain_out + emit_close + reap.
    - http3_callbacks.c   : every nghttp3 + ngtcp2 callback, dispatch
                            tables, init_h3, submit_response.
    - http3_dispatch.c    : per-stream user-handler coroutine.

  Cross-TU contract lives in http3_internal.h.
*/

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include "http3_internal.h"               /* php.h + Zend/zend_async_API.h +
                                            * ngtcp2 + nghttp3 + openssl/ssl.h +
                                            * http3_connection.h + php_http_server.h */
#include "Zend/zend_hrtime.h"              /* zend_hrtime — drain stamps */
#include "http3_listener.h"                /* listener accessors */
#include "http3_packet.h"                  /* version_negotiation / stateless_reset */
#include "http3/http3_stream.h"            /* http3_stream_t (callbacks.c symmetry) */

#include <ngtcp2/ngtcp2_crypto.h>          /* recv_client_initial / hp_mask */
#include <ngtcp2/ngtcp2_crypto_ossl.h>     /* per-conn TLS attach */
#include <openssl/err.h>                   /* ERR_print_errors_fp on debug */
#include <openssl/rand.h>                  /* RAND_bytes */

#include "../core/tls_layer.h"             /* tls_layer_mark_ssl_quic */
#include "log/http_log.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#ifdef PHP_WIN32
# include <winsock2.h>
# include <ws2tcpip.h>
#else
# include <sys/socket.h>
# include <netinet/in.h>
# include <arpa/inet.h>
#endif

/* Listener accessors implemented in http3_listener.c. */
extern http3_packet_stats_t *http3_listener_packet_stats(http3_listener_t *l);
extern HashTable *http3_listener_conn_map(http3_listener_t *l);
extern void http3_listener_track_connection(
    http3_listener_t *l, http3_connection_t *conn);
extern const uint8_t *http3_listener_sr_key(const http3_listener_t *l);
extern const uint8_t *http3_listener_retry_token_key(const http3_listener_t *l);
extern bool http3_listener_peer_inc(http3_listener_t *l,
                                    const struct sockaddr *peer);
extern void http3_listener_peer_dec(http3_listener_t *l,
                                    const struct sockaddr *peer);
extern void *http3_listener_ssl_ctx(http3_listener_t *l);
extern const char *http3_listener_host(const http3_listener_t *l);
extern int http3_listener_port(const http3_listener_t *l);

/* ------------------------------------------------------------------------
 * Process-wide one-shot helpers
 * ------------------------------------------------------------------------ */

/* ngtcp2_crypto_ossl_init is an optional one-shot global initialiser —
 * idempotent per ngtcp2 docs. Calling it eagerly buys the fast paths that
 * benchmarking QUIC requires to be meaningful. The atomic guard makes the
 * ensure_*() call race-free under PHP-ZTS where two worker threads may
 * both accept their first H3 connection simultaneously. */
static zend_atomic_bool ossl_crypto_initialised = {0};

void http3_ensure_ossl_crypto_init(void)
{
    /* exchange(true) returns previous value — only the first caller
     * actually runs the init. ngtcp2 is idempotent, but avoiding the
     * second call is cheaper. */
    if (!zend_atomic_bool_exchange(&ossl_crypto_initialised, true)) {
        (void)ngtcp2_crypto_ossl_init();
    }
}

/* Secure random bytes via OpenSSL. RAND_bytes draws from the same
 * provider-backed DRBG that TLS 1.3 uses for its own keys — no reason to
 * drag in a separate platform RNG path. */
bool http3_fill_random(uint8_t *buf, size_t len)
{
    return RAND_bytes(buf, (int)len) == 1;
}

/* Local timestamp in ngtcp2 format (nanoseconds since some monotonic
 * epoch). ngtcp2 does not care about the epoch as long as we are
 * consistent — zend_hrtime fits. */
ngtcp2_tstamp http3_ts_now(void)
{
    return (ngtcp2_tstamp) zend_hrtime();
}

/* Bridges ngtcp2's variadic log_printf into http_log at DEBUG. Volume
 * is high (one line per frame); installed only when DEBUG is active.
 * ngtcp2 passes the conn's user_data here, which we set to the
 * h3_connection_t at handshake time. */
void http3_debug_logger(void *user_data, const char *fmt, ...)
{
    http3_connection_t *c = (http3_connection_t *)user_data;
    http_log_state_t *st = (c != NULL) ? c->log_state : NULL;
    if (st == NULL || st->severity == HTTP_LOG_OFF
        || (int)HTTP_LOG_DEBUG < (int)st->severity) {
        return;
    }
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0) {
        return;
    }
    http_logf_debug(st, "h3.ngtcp2 %s", buf);
}

/* Build a sockaddr_storage from the listener's bound (host, port).
 *
 * ngtcp2_path matching is strict: the local addr passed to read_pkt /
 * writev_stream must be the same value on every call after server_new.
 * The proper plumbing for this is `zend_async_udp_sockname` (parked as
 * a Step-6 upstream blocker per project_http3_progress.md); until that
 * lands we fabricate the sockaddr from the bind config so at least it
 * is stable across calls. peer_family lets us produce v4 / v6 to match
 * the inbound datagram. Returns 0 on success. */
int http3_build_listener_local(const http3_listener_t *l,
                               int peer_family,
                               struct sockaddr_storage *out,
                               socklen_t *out_len)
{
    memset(out, 0, sizeof(*out));
    const char *host = http3_listener_host(l);
    int port = http3_listener_port(l);
    if (host == NULL) host = (peer_family == AF_INET6) ? "::" : "0.0.0.0";

    if (peer_family == AF_INET6) {
        struct sockaddr_in6 *s6 = (struct sockaddr_in6 *)out;
        s6->sin6_family = AF_INET6;
        s6->sin6_port   = htons((uint16_t)port);
        if (inet_pton(AF_INET6, host, &s6->sin6_addr) != 1) {
            /* Listener bound to v4-only host but peer is v6 — use ::1. */
            inet_pton(AF_INET6, "::1", &s6->sin6_addr);
        }
        *out_len = sizeof(*s6);
    } else {
        struct sockaddr_in *s4 = (struct sockaddr_in *)out;
        s4->sin_family = AF_INET;
        s4->sin_port   = htons((uint16_t)port);
        if (inet_pton(AF_INET, host, &s4->sin_addr) != 1) {
            s4->sin_addr.s_addr = htonl(INADDR_ANY);
        }
        *out_len = sizeof(*s4);
    }
    return 0;
}

/* ------------------------------------------------------------------------
 * TLS attach (per-connection SSL bound to ngtcp2 via crypto_ossl)
 * ------------------------------------------------------------------------ */

/* ngtcp2 calls this via SSL's app_data to obtain the owning ngtcp2_conn.
 * Paired with SSL_set_app_data(ssl, &conn->crypto_conn_ref) at accept. */
static ngtcp2_conn *get_conn_from_ssl(ngtcp2_crypto_conn_ref *ref)
{
    http3_connection_t *c = (http3_connection_t *)ref->user_data;
    return c != NULL ? (ngtcp2_conn *)c->ngtcp2_conn : NULL;
}

/* Build the per-connection TLS state: fresh SSL from the shared SSL_CTX,
 * configured for server-side QUIC via ngtcp2_crypto_ossl_configure_server_
 * session, wired back to ngtcp2_conn through crypto_conn_ref. On any
 * failure, partial state is rolled back and the function returns false —
 * the caller must dispose the ngtcp2_conn + struct it already allocated. */
static bool http3_connection_attach_tls(http3_connection_t *c, SSL_CTX *ctx)
{
    if (ctx == NULL) {
        return false;
    }
    http3_ensure_ossl_crypto_init();

    SSL *ssl = SSL_new(ctx);
    if (ssl == NULL) {
        return false;
    }
    /* Mark this SSL handle as the QUIC side of the shared SSL_CTX so the
     * ALPN selector advertises "h3" instead of the TCP list. Must happen
     * before any handshake activity. */
    tls_layer_mark_ssl_quic(ssl);
    if (ngtcp2_crypto_ossl_configure_server_session(ssl) != 0) {
        http_log_state_t *st = (c != NULL) ? c->log_state : NULL;
        http_logf_debug(st, "h3.configure_server_session.failed");
        if (st != NULL && (int)HTTP_LOG_DEBUG >= (int)st->severity
            && st->severity != HTTP_LOG_OFF) {
            ERR_print_errors_fp(stderr);
        }
        SSL_free(ssl);
        return false;
    }

    /* ngtcp2's SSL app_data points to a crypto_conn_ref; the ref's
     * get_conn callback returns the ngtcp2_conn*. Ref lifetime is tied
     * to the struct (struct free releases ref too). */
    ngtcp2_crypto_conn_ref *ref = ecalloc(1, sizeof(*ref));
    ref->get_conn  = get_conn_from_ssl;
    ref->user_data = c;
    SSL_set_app_data(ssl, ref);
    SSL_set_accept_state(ssl);

    /* ngtcp2_crypto_ossl_ctx owns the SSL from here: ngtcp2_crypto_ossl_
     * ctx_del frees SSL too. */
    ngtcp2_crypto_ossl_ctx *octx = NULL;
    if (ngtcp2_crypto_ossl_ctx_new(&octx, ssl) != 0) {
        SSL_set_app_data(ssl, NULL);
        OPENSSL_cleanse(ref, sizeof(*ref));
        efree(ref);
        SSL_free(ssl);
        return false;
    }

    ngtcp2_conn_set_tls_native_handle((ngtcp2_conn *)c->ngtcp2_conn, octx);

    c->ssl             = ssl;
    c->crypto_ctx      = octx;
    c->crypto_conn_ref = ref;
    return true;
}

/* ------------------------------------------------------------------------
 * Connection accept (Initial → ngtcp2_conn_server_new + transport params)
 * ------------------------------------------------------------------------ */

static http3_connection_t *http3_connection_accept(
    http3_listener_t *listener,
    const ngtcp2_pkt_hd *hd,
    const struct sockaddr *peer, socklen_t peer_len,
    const uint8_t *retry_odcid, size_t retry_odcid_len)
{
    /* Per-peer budget gate. Apply BEFORE we allocate any
     * ngtcp2/SSL state so a flooding peer can't burn server memory
     * waiting for the cap to bite. Inc on success → matching dec lives
     * in http3_connection_free. */
    if (!http3_listener_peer_inc(listener, peer)) {
        http3_packet_stats_t *stats = http3_listener_packet_stats(listener);
        if (stats != NULL) stats->quic_conn_per_peer_rejected++;
        return NULL;
    }

    http3_connection_t *c = ecalloc(1, sizeof(http3_connection_t));
    c->listener = listener;
    /* Cache hot-path slices. http3_listener_server_obj() returns NULL
     * for an unparented listener — accessor handles that, returning the
     * dummy / default fallbacks. */
    {
        http_server_object *srv =
            (http_server_object *)http3_listener_server_obj(listener);
        c->counters  = http_server_counters(srv);
        c->view      = http_server_view(srv);
        c->log_state = http_server_get_log_state(srv);
    }

    /* original_dcid for transport_params: with Retry, this is the DCID
     * from the FIRST Initial (recovered from the verified Retry token);
     * without Retry, it is the current Initial's DCID. */
    if (retry_odcid != NULL && retry_odcid_len > 0) {
        memcpy(c->original_dcid, retry_odcid, retry_odcid_len);
        c->original_dcidlen = retry_odcid_len;
    } else {
        memcpy(c->original_dcid, hd->dcid.data, hd->dcid.datalen);
        c->original_dcidlen = hd->dcid.datalen;
    }

    /* Generate our own SCID; subsequent client packets address us with
     * this as their DCID. A DRBG failure here must NOT fall through to
     * a zero SCID (would collide in conn_map with any other conn whose
     * SCID generation also failed) — fail the accept cleanly instead. */
    if (!http3_fill_random(c->scid, HTTP3_SCID_LEN)) {
        http3_listener_peer_dec(listener, peer);
        OPENSSL_cleanse(c, sizeof(*c));
        efree(c);
        return NULL;
    }
    c->scidlen = HTTP3_SCID_LEN;

    /* Clamp to sockaddr_storage — a malformed peer_len from a hostile
     * reactor would otherwise overflow our storage. The reactor today
     * always passes struct sockaddr_in/in6 sizes, but we are the last
     * line of defence against a future buggy UDP backend. */
    socklen_t clamped = (peer_len > (socklen_t)sizeof(c->peer))
                        ? (socklen_t)sizeof(c->peer) : peer_len;
    memcpy(&c->peer, peer, (size_t)clamped);
    c->peer_len = clamped;

    ngtcp2_cid dcid;
    memcpy(dcid.data, hd->scid.data, hd->scid.datalen);
    dcid.datalen = hd->scid.datalen;

    ngtcp2_cid scid;
    memcpy(scid.data, c->scid, c->scidlen);
    scid.datalen = c->scidlen;

    ngtcp2_cid orig_dcid;
    memcpy(orig_dcid.data, c->original_dcid, c->original_dcidlen);
    orig_dcid.datalen = c->original_dcidlen;

    /* Stable local sockaddr derived from the listener bind config. See
     * http3_build_listener_local() — works around the missing
     * zend_async_udp_sockname API. */
    struct sockaddr_storage local_addr;
    socklen_t local_addr_len = 0;
    http3_build_listener_local(listener, peer->sa_family, &local_addr, &local_addr_len);

    ngtcp2_path path = {
        .local  = { .addr = (struct sockaddr *)&local_addr, .addrlen = local_addr_len },
        .remote = { .addr = (struct sockaddr *)&c->peer,    .addrlen = (ngtcp2_socklen)peer_len },
        .user_data = NULL,
    };

    ngtcp2_settings settings;
    ngtcp2_settings_default(&settings);
    settings.initial_ts = http3_ts_now();
    /* Bound the pre-handshake window. ngtcp2's default is
     * UINT64_MAX, which lets a peer hold a half-open conn (and the
     * associated ngtcp2_conn + SSL state) for as long as it likes —
     * a textbook slow-loris vector. 10s is the same envelope the TCP
     * accept path uses for read-header timeouts in this server. */
    settings.handshake_timeout = 10 * NGTCP2_SECONDS;
    if (c != NULL && c->log_state != NULL
        && c->log_state->severity != HTTP_LOG_OFF
        && (int)HTTP_LOG_DEBUG >= (int)c->log_state->severity) {
        settings.log_printf = http3_debug_logger;
    }

    /* Transport params resolution (NEXT_STEPS.md §5):
     *   1. HttpServerConfig setters at server start() time.
     *   2. PHP_HTTP3_BENCH_FC=1 raises FC + streams to bench-grade
     *      values (NEVER enable in production — disables back-pressure).
     *   3. PHP_HTTP3_IDLE_TIMEOUT_MS env override (legacy ops hatch,
     *      no upper ceiling now that the API exposes the same knob).
     *   4. Built-in defaults if config left a field at 0 / unset.
     *
     * `initial_max_data` is derived as `stream_window × max_streams_bidi`
     * (nginx pattern); we deliberately do NOT expose a separate setter,
     * because letting the two drift independently produces back-pressure
     * surprises (huge per-stream window + tiny conn cap = stall). */
    const http_server_object *srv_obj =
        (const http_server_object *)http3_listener_server_obj(listener);

    uint32_t cfg_window  = http_server_get_http3_stream_window_bytes(srv_obj);
    uint32_t cfg_streams = http_server_get_http3_max_concurrent_streams(srv_obj);
    uint32_t cfg_idle_ms = http_server_get_http3_idle_timeout_ms(srv_obj);

    uint64_t window_bytes = cfg_window != 0 ? (uint64_t)cfg_window : (256ull * 1024);
    uint64_t streams_bidi = cfg_streams != 0 ? (uint64_t)cfg_streams : 100;
    uint64_t idle_ms      = cfg_idle_ms != 0 ? (uint64_t)cfg_idle_ms : 30000;

    if (getenv("PHP_HTTP3_BENCH_FC") != NULL) {
        /* Bench escape hatch: raises window + streams to bench-grade
         * values regardless of config. Composes on top of the new API
         * so existing bench harnesses keep working unchanged. */
        window_bytes = 16ull * 1024 * 1024;
        streams_bidi = 1000000ull;
    }

    /* Idle-timeout env override stays for ops, but the cap now matches
     * the config-API ceiling (UINT32_MAX) — same shape as the setter. */
    {
        const char *env = getenv("PHP_HTTP3_IDLE_TIMEOUT_MS");
        if (env != NULL && *env != '\0') {
            char *end = NULL;
            unsigned long ms = strtoul(env, &end, 10);
            if (end != env && *end == '\0' && ms > 0 && ms <= UINT32_MAX) {
                idle_ms = (uint64_t)ms;
            }
        }
    }

    ngtcp2_transport_params params;
    ngtcp2_transport_params_default(&params);
    params.initial_max_stream_data_bidi_local  = window_bytes;
    params.initial_max_stream_data_bidi_remote = window_bytes;
    params.initial_max_stream_data_uni         = window_bytes;
    /* nginx-style derived conn FC: per-stream window × max bidi streams.
     * Saturates at UINT64_MAX in the pathological 1 GiB × 1M case. */
    params.initial_max_data    = window_bytes * streams_bidi;
    params.initial_max_streams_bidi = streams_bidi;
    params.initial_max_streams_uni  = 3;
    if (getenv("PHP_HTTP3_BENCH_FC") != NULL) {
        params.initial_max_streams_uni = 100;
    }
    params.max_idle_timeout = (ngtcp2_duration)idle_ms * NGTCP2_MILLISECONDS;
    params.active_connection_id_limit = 7;
    params.original_dcid = orig_dcid;
    params.original_dcid_present = 1;
    /* When this Initial arrived after a Retry, the client's current DCID
     * equals the SCID we picked in the Retry packet — advertise it to
     * the client so it can authenticate that we are the same server
     * that issued the Retry. */
    if (retry_odcid != NULL && retry_odcid_len > 0) {
        ngtcp2_cid retry_scid;
        memcpy(retry_scid.data, hd->dcid.data, hd->dcid.datalen);
        retry_scid.datalen = hd->dcid.datalen;
        params.retry_scid = retry_scid;
        params.retry_scid_present = 1;
    }

    ngtcp2_conn *qc = NULL;
    int rv = ngtcp2_conn_server_new(
        &qc, &dcid, &scid, &path, hd->version,
        &HTTP3_NGTCP2_CALLBACKS, &settings, &params,
        NULL, c);

    if (rv != 0) {
        http3_listener_peer_dec(listener, peer);
        OPENSSL_cleanse(c, sizeof(*c));
        efree(c);
        return NULL;
    }

    c->ngtcp2_conn = qc;

    /* Attach TLS. Without this, ngtcp2_conn_read_pkt fails on
     * the first CRYPTO frame. The SSL_CTX* is shared with the TCP/H2
     * listener — same cert/key, same ALPN callback that now also
     * accepts "h3". */
    if (!http3_connection_attach_tls(c, (SSL_CTX *)http3_listener_ssl_ctx(listener))) {
        http3_listener_peer_dec(listener, peer);
        ngtcp2_conn_del(qc);
        OPENSSL_cleanse(c, sizeof(*c));
        efree(c);
        return NULL;
    }

    /* Publish in the listener's DCID map under both keys:
     *
     *   - our SCID: the DCID the client will use in short-header (1-RTT)
     *     packets once handshake progresses.
     *   - client's original DCID: the address on every retransmit of the
     *     Initial packet before the server's SCID has been learned.
     *     Without this key, a duplicate Initial would miss the map,
     *     ngtcp2_conn_server_new would run again, and we would accrue a
     *     fresh connection per retransmit — an amplification vector.
     *
     * Both lookups point to the same http3_connection_t; the hashtable
     * is non-owning so the double-key does not double-free on teardown. */
    HashTable *map = http3_listener_conn_map(listener);
    if (map != NULL) {
        zend_hash_str_add_ptr(map, (const char *)c->scid, c->scidlen, c);
        /* original_dcid == scid would be a degenerate 8-byte collision —
         * skip the second add_ptr so hashtable_add_new does not error. */
        if (c->original_dcidlen != c->scidlen
            || memcmp(c->original_dcid, c->scid, c->scidlen) != 0) {
            zend_hash_str_add_ptr(map,
                (const char *)c->original_dcid, c->original_dcidlen, c);
        }
    }

    /* Stamp graceful-drain state. Mirror of the H1/H2
     * http_connection_t init in src/core/http_connection.c — keeps the
     * drain semantics protocol-uniform (proactive age + ±10% jitter,
     * reactive epoch picked up on the next response commit). */
    c->created_at_ns       = (uint64_t)zend_hrtime();
    c->drain_pending       = false;
    c->drain_submitted     = false;
    c->drain_epoch_seen    = 0;
    c->drain_not_before_ns = UINT64_MAX;
    {
        http_server_object *srv =
            (http_server_object *)http3_listener_server_obj(listener);
        const uint64_t base = http_server_get_max_connection_age_ns(srv);
        if (base > 0) {
            const uint64_t h          = (uintptr_t)c * 2654435761ULL;
            const uint64_t twenty_pct = base / 5;
            const uint64_t jitter     = twenty_pct > 0 ? (h % twenty_pct) : 0;
            const uint64_t offset     = (base - twenty_pct / 2) + jitter;
            c->drain_not_before_ns    = c->created_at_ns + offset;
        }
    }

    /* Own through the intrusive list — teardown walks this, not the map.
     * Add after map registration so a fragment state (listed but not
     * looked up) cannot be observed. */
    http3_listener_track_connection(listener, c);

    return c;
}

/* ------------------------------------------------------------------------
 * Packet entry — listener calls this for each inbound datagram
 * ------------------------------------------------------------------------ */

bool http3_connection_dispatch(
    http3_listener_t *listener,
    const uint8_t *data, size_t datalen, uint8_t ecn,
    const struct sockaddr *peer, socklen_t peer_len)
{
    if (listener == NULL || data == NULL || datalen == 0) {
        return false;
    }

    http3_packet_stats_t *stats = http3_listener_packet_stats(listener);
    if (stats == NULL) {
        return false;
    }

    ngtcp2_version_cid vc = {0};
    int rv = ngtcp2_pkt_decode_version_cid(&vc, data, datalen, HTTP3_SCID_LEN);

    if (rv == NGTCP2_ERR_VERSION_NEGOTIATION) {
        if (http3_packet_send_version_negotiation(
                listener, vc.dcid, vc.dcidlen, vc.scid, vc.scidlen,
                peer, peer_len)) {
            stats->quic_version_negotiated++;
        }
        return true;
    }
    if (rv < 0) {
        stats->quic_parse_errors++;
        return false;
    }

    HashTable *map = http3_listener_conn_map(listener);
    http3_connection_t *conn = map != NULL
        ? zend_hash_str_find_ptr(map, (const char *)vc.dcid, vc.dcidlen)
        : NULL;

    if (conn == NULL) {
        /* No matching connection. For a long-header (version != 0) INITIAL
         * this is expected — we create one. For a short-header (version == 0)
         * it is a 1-RTT packet for a conn we do not know: answer with a
         * stateless reset (RFC 9000 §10.3). The helper enforces the
         * 1× anti-amplification rule; tiny forged probes are dropped silently. */
        if (vc.version == 0) {
            stats->quic_short_header++;
            const uint8_t *sr_key = http3_listener_sr_key(listener);
            if (sr_key != NULL && vc.dcidlen > 0) {
                if (http3_packet_send_stateless_reset(
                        listener, sr_key,
                        vc.dcid, vc.dcidlen,
                        datalen, peer, peer_len)) {
                    stats->quic_stateless_reset_sent++;
                }
            }
            return true;
        }

        /* Parse the long-header fully to get packet type + token for
         * ngtcp2_conn_server_new. */
        ngtcp2_pkt_hd hd;
        ngtcp2_ssize n = ngtcp2_accept(&hd, data, datalen);
        if (n < 0) {
            /* Parseable by pkt_decode_version_cid but not a valid
             * INITIAL — could be a HANDSHAKE or 0-RTT from a forgotten
             * conn, or a malformed packet. Count as parse error. */
            stats->quic_parse_errors++;
            return false;
        }

        /* RFC 9000 §8.1.2 source-address validation. An attacker with a
         * spoofed source IP can otherwise force us to allocate
         * ngtcp2_conn + SSL state and emit ServerHello+cert chain
         * (~3× amplification) at the spoofed victim. Force a Retry on
         * the first Initial; only allocate state once the client
         * proves it can read at its claimed address by echoing back
         * our token. */
        const uint8_t *retry_key = http3_listener_retry_token_key(listener);
        uint8_t odcid_buf[NGTCP2_MAX_CIDLEN];
        size_t  odcid_buf_len = 0;
        bool    have_odcid    = false;

        /* Operator escape hatch — same shape as PHP_HTTP3_BENCH_FC.
         * Disables source-address validation (Retry) entirely. Used by
         * the embedded h3client test harness which is single-shot and
         * does not implement the Retry round-trip. NEVER set in
         * production: it re-opens the §8.1.2 amplification gap. */
        const bool retry_disabled =
            getenv("PHP_HTTP3_DISABLE_RETRY") != NULL;

        if (retry_key != NULL && !retry_disabled) {
            if (hd.tokenlen == 0) {
                /* No token → emit Retry and drop the Initial. */
                if (http3_packet_send_retry(
                        listener, retry_key, hd.version,
                        vc.dcid, vc.dcidlen, vc.scid, vc.scidlen,
                        peer, peer_len)) {
                    stats->quic_retry_sent++;
                }
                return true;
            }
            /* Token present — only accept Retry-shaped tokens. Regular
             * NEW_TOKEN tokens (NGTCP2_CRYPTO_TOKEN_MAGIC_REGULAR) are
             * not yet supported here; reject them so we still force
             * address validation rather than trusting the peer. */
            if (hd.token[0] != NGTCP2_CRYPTO_TOKEN_MAGIC_RETRY) {
                stats->quic_retry_token_invalid++;
                return true;
            }
            const int vrv = http3_packet_verify_retry_token(
                retry_key, hd.version, hd.token, hd.tokenlen,
                vc.dcid, vc.dcidlen, peer, peer_len,
                odcid_buf, &odcid_buf_len);
            if (vrv != 0) {
                stats->quic_retry_token_invalid++;
                return true;
            }
            stats->quic_retry_token_ok++;
            have_odcid = true;
        }

        conn = http3_connection_accept(
            listener, &hd, peer, peer_len,
            have_odcid ? odcid_buf      : NULL,
            have_odcid ? odcid_buf_len  : 0);
        if (conn == NULL) {
            stats->quic_initial++;
            stats->quic_conn_rejected++;
            return true;
        }
        stats->quic_conn_accepted++;
    }

    if (vc.version != 0) {
        stats->quic_initial++;
    } else {
        stats->quic_short_header++;
    }

    /* Feed the datagram into the connection. ngtcp2 drives TLS internally
     * via the crypto_ossl callbacks we wired at accept time. On the first
     * Initial this kicks off the ServerHello + EncryptedExtensions path;
     * subsequent packets advance the handshake or carry 1-RTT data.
     *
     * Same fabricated local addr as accept / drain — see
     * http3_build_listener_local() for the rationale. */
    struct sockaddr_storage local_addr;
    socklen_t local_addr_len = 0;
    http3_build_listener_local(listener, conn->peer.ss_family,
                               &local_addr, &local_addr_len);

    ngtcp2_path_storage ps = {0};
    ngtcp2_path_storage_init(&ps,
        (const struct sockaddr *)&local_addr, local_addr_len,
        (const struct sockaddr *)&conn->peer, conn->peer_len,
        NULL);

    /* Forward ECN bits from cmsg into ngtcp2 — the only field of
     * pkt_info that read_pkt actually consumes today. ngtcp2 masks to
     * the 2 ECN bits internally; we pass the full TOS byte through. */
    ngtcp2_pkt_info pi = { .ecn = ecn };
    int pkt_rv = ngtcp2_conn_read_pkt(
        (ngtcp2_conn *)conn->ngtcp2_conn,
        &ps.path, &pi,
        data, datalen, http3_ts_now());

    if (UNEXPECTED(pkt_rv != 0)) {
        http_log_state_t *st = conn->log_state;
        http_logf_debug(st, "h3.read_pkt.failed rv=%d datalen=%zu",
                        pkt_rv, datalen);
        if (st != NULL && st->severity != HTTP_LOG_OFF
            && (int)HTTP_LOG_DEBUG >= (int)st->severity) {
            ERR_print_errors_fp(stderr);
        }
    }

    if (pkt_rv == 0) {
        stats->quic_read_ok++;
    } else if (pkt_rv == NGTCP2_ERR_DRAINING ||
               pkt_rv == NGTCP2_ERR_CLOSING ||
               pkt_rv == NGTCP2_ERR_CRYPTO ||
               pkt_rv == NGTCP2_ERR_RETRY) {
        stats->quic_read_error++;
    } else {
        stats->quic_read_fatal++;
    }

    /* Flush any outgoing handshake/control packets ngtcp2 has
     * produced as a consequence of processing this read, then re-arm the
     * retransmission/PTO timer so missing ACKs still trigger retries. We
     * drain even on read errors because ngtcp2 may want to emit a
     * CONNECTION_CLOSE frame explaining the failure to the peer. */
    http3_connection_drain_out(conn);
    /* read_pkt may have moved the connection into closing or
     * draining (peer-initiated close, or transport/crypto error). Reap
     * before arming the timer — the retransmission timer is meaningless
     * in those states. check_terminal returns true after freeing conn,
     * so we MUST NOT touch it on the true branch. */
    if (http3_connection_check_terminal(conn)) {
        return true;
    }
    http3_connection_arm_timer(conn);

    return true;
}

/* ------------------------------------------------------------------------
 * Connection free — teardown order
 * ------------------------------------------------------------------------ */

void http3_connection_free(http3_connection_t *conn)
{
    if (UNEXPECTED(conn == NULL || conn->closed)) {
        return;
    }

    /* Best-effort graceful CONNECTION_CLOSE before teardown.
     * No-op if we already emitted one (closing/draining reap path) or
     * if ngtcp2 is in the draining period (peer initiated). The TRY_SEND
     * path is synchronous so the datagram leaves the socket before we
     * proceed to release ngtcp2/SSL state. */
    http3_connection_emit_close(conn);

    /* Release the per-peer-IP slot we claimed at accept. The
     * peer sockaddr is still intact at this point (efree happens at
     * the very end). */
    http3_listener_peer_dec(conn->listener,
                            (const struct sockaddr *)&conn->peer);

    conn->closed = true;

    /* Tear down the retransmission timer first — its callback references
     * this connection, and firing after closed=true would be a UAF on
     * the conn->ngtcp2_conn we are about to ngtcp2_conn_del. The guard
     * in timer_fire_cb handles racing invocations already queued but
     * not yet delivered. */
    http3_connection_detach_timer(conn);

    /* Teardown order per ngtcp2_crypto_ossl docs:
     *  1. Detach TLS handle from ngtcp2_conn so its dtor does not touch SSL.
     *  2. Clear SSL app_data — the crypto_conn_ref lifetime ends below.
     *  3. ngtcp2_crypto_ossl_ctx_del — implicitly SSL_free.
     *  4. ngtcp2_conn_del.
     * nghttp3_conn must be released before ngtcp2_conn_del so its
     * stream-state references (exposed via stream_user_data) don't
     * dangle. nghttp3 has no back-reference to ngtcp2, so the order is
     * one-way safe. */
    /* Force-release any stream nghttp3_conn_del would otherwise
     * orphan: it does not fire stream_close on remaining streams, so
     * each one's request + headers + zend_strings would leak. Walk our
     * live-stream list, clear the framing-layer back-pointers (so any
     * straggler callback that fires on the way out cannot deref a
     * dying stream), and drop the user_data ref. NULL the head first
     * so the per-stream unlink path inside http3_stream_release short-
     * circuits. */
    {
        http3_stream_t *s = conn->streams_head;
        conn->streams_head = NULL;
        while (s != NULL) {
            http3_stream_t *next = s->list_next;
            s->conn = NULL;
            if (conn->nghttp3_conn != NULL) {
                nghttp3_conn_set_stream_user_data(
                    (nghttp3_conn *)conn->nghttp3_conn, s->stream_id, NULL);
            }
            if (conn->ngtcp2_conn != NULL) {
                ngtcp2_conn_set_stream_user_data(
                    (ngtcp2_conn *)conn->ngtcp2_conn, s->stream_id, NULL);
            }
            http3_stream_release(s);
            s = next;
        }
    }
    if (conn->nghttp3_conn != NULL) {
        nghttp3_conn_del((nghttp3_conn *)conn->nghttp3_conn);
        conn->nghttp3_conn = NULL;
    }
    if (conn->ngtcp2_conn != NULL) {
        ngtcp2_conn_set_tls_native_handle((ngtcp2_conn *)conn->ngtcp2_conn, NULL);
    }
    if (conn->ssl != NULL) {
        SSL_set_app_data((SSL *)conn->ssl, NULL);
    }
    if (conn->crypto_conn_ref != NULL) {
        /* Cast through the concrete type so MSVC accepts sizeof(*p) — GCC
         * folds sizeof(void) to 1 silently, MSVC errors out (C2100). */
        OPENSSL_cleanse(conn->crypto_conn_ref,
                        sizeof(*(ngtcp2_crypto_conn_ref *)conn->crypto_conn_ref));
        efree(conn->crypto_conn_ref);
        conn->crypto_conn_ref = NULL;
    }
    if (conn->crypto_ctx != NULL) {
        ngtcp2_crypto_ossl_ctx_del((ngtcp2_crypto_ossl_ctx *)conn->crypto_ctx);
        conn->crypto_ctx = NULL;
    }
    /* ngtcp2_crypto_ossl_ctx_del does NOT free the SSL object it holds a
     * pointer to (docs are silent; source at crypto/ossl/ossl.c confirms).
     * We own the SSL — created via SSL_new — and must release it here. */
    if (conn->ssl != NULL) {
        SSL_free((SSL *)conn->ssl);
        conn->ssl = NULL;
    }
    if (conn->ngtcp2_conn != NULL) {
        ngtcp2_conn_del((ngtcp2_conn *)conn->ngtcp2_conn);
        conn->ngtcp2_conn = NULL;
    }
    /* Wipe the struct shell before returning to ZendMM. Holds random
     * SCID/original_dcid, peer sockaddr, and pointer slots whose values
     * (now stale) leak heap layout to a UAF reader. External symbol —
     * survives DSE at -O2; see docs/AUDIT_HTTP3_STEP3.md. */
    OPENSSL_cleanse(conn, sizeof(*conn));
    efree(conn);
}

