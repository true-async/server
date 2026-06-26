/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

#ifndef HTTP3_CONNECTION_H
#define HTTP3_CONNECTION_H

/* Declarations are always visible; implementation is guarded by
 * HAVE_HTTP_SERVER_HTTP3 inside the .c file. This matches the pattern in
 * http3_listener.h and keeps header-ordering safe across TUs. */

#include <zend.h>
#include <stdint.h>
#include "php_http_server.h"   /* http_server_counters_t / http_server_view_t */
/* Pull in struct sockaddr / sockaddr_storage / socklen_t in a way that
 * works on Linux, macOS and Windows. winsock2.h must precede any other
 * Windows header that drags in windows.h, hence the guard. */
#ifdef PHP_WIN32
# include <winsock2.h>
# include <ws2tcpip.h>
#else
# include <sys/socket.h>
#endif

typedef struct _http3_listener_s   http3_listener_t;    /* defined in http3_listener.h */
typedef struct _http3_connection_s http3_connection_t;
typedef struct _http3_stream_s     http3_stream_t;      /* defined in http3_stream.h */

/* One server-issued alternate CID (NEW_CONNECTION_ID, RFC 9000 §5.1). */
typedef struct {
    uint8_t data[20];   /* NGTCP2_MAX_CIDLEN */
    size_t  len;
} http3_issued_cid_t;

/* Negotiated QUIC application protocol. h3 (zero default) drives nghttp3;
 * hq-interop is the raw HTTP/0.9-over-QUIC interop shim (no nghttp3). */
typedef enum {
    HTTP3_PROTO_H3 = 0,
    HTTP3_PROTO_HQ
} http3_proto_t;

/* Per-connection state.
 *
 * One http3_connection_t per QUIC connection, identified by the server-
 * chosen SCID (which is what the client uses as DCID in follow-up
 * datagrams). The listener owns a HashTable keyed by those SCID bytes. */
struct _http3_connection_s {
    /* Server-chosen SCID, 8 bytes — we'll use this as the key in the
     * listener's DCID map. Client addresses this connection with it as
     * their DCID in every subsequent packet. */
    uint8_t  scid[20];          /* NGTCP2_MAX_CIDLEN */
    size_t   scidlen;

    /* Original DCID from the client's first INITIAL — kept for debug
     * and for transport_params.original_dcid. */
    uint8_t  original_dcid[20];
    size_t   original_dcidlen;

    /* The DCID the client actually used in the INITIAL that created this
     * connection (== original_dcid without a Retry; == the Retry's SCID
     * after one). conn_map is keyed on this so a retransmitted INITIAL
     * routes back here instead of re-entering the accept path. 0 length
     * means "same as scid/original_dcid, no extra key registered". */
    uint8_t  routing_dcid[20];
    size_t   routing_dcidlen;

    /* Server-issued alternate CIDs handed to the client via
     * NEW_CONNECTION_ID (RFC 9000 §5.1). The client may rotate its DCID to
     * any of them, so each is registered in the listener conn_map and MUST be
     * unregistered before this conn is freed. Tracked explicitly here — NOT
     * derived from ngtcp2_conn_get_scid — because ngtcp2 keeps a retired CID
     * in its pool for ~3*PTO after RETIRE_CONNECTION_ID before it fires
     * remove_connection_id, a window in which get_scid omits a CID still live
     * in conn_map. Membership is mutated only at our own register/unregister
     * points, so teardown is timing-independent. Dynamic; bounded in practice
     * by ngtcp2's SCID pool. */
    http3_issued_cid_t *issued_cids;
    size_t              issued_cid_count;
    size_t              issued_cid_cap;

    /* Peer address — the live send path. Re-pointed to a new client path on
     * migration / NAT rebind (RFC 9000 §9), so drain_out follows it. */
    struct sockaddr_storage peer;
    socklen_t               peer_len;

    /* The address that paid the per-peer admission slot at accept (IP-keyed,
     * no length needed), so teardown frees the right bucket post-migration. */
    struct sockaddr_storage admit_peer;

    /* Opaque ngtcp2_conn pointer. void* to keep ngtcp2 types out of this
     * header — the concrete type is ngtcp2_conn *. Accessed via
     * http3_connection_ngtcp2(). */
    void *ngtcp2_conn;

    /* Opaque nghttp3_conn (HTTP/3 framing layer). NULL until
     * the TLS handshake completes; created in handshake_completed_cb so
     * that it is only ever attached to a verified-h3 peer. Stays NULL for
     * an hq-interop peer (raw HTTP/0.9, no framing layer). */
    void *nghttp3_conn;

    /* ALPN settled by handshake_completed_cb. Zero (H3) is the default. */
    http3_proto_t proto;

    /* Per-connection TLS state. All three owned here; teardown order:
     *  1. ngtcp2_conn_set_tls_native_handle(conn, NULL)
     *  2. SSL_set_app_data(ssl, NULL)  ← ngtcp2 docs REQUIRE this if
     *     ngtcp2_conn might be freed before SSL_free.
     *  3. ngtcp2_crypto_ossl_ctx_del(crypto_ctx)  ← implicitly SSL_free
     *  4. ngtcp2_conn_del(conn) */
    void *ssl;                  /* SSL* */
    void *crypto_ctx;           /* ngtcp2_crypto_ossl_ctx* */
    void *crypto_conn_ref;      /* ngtcp2_crypto_conn_ref* — app_data on SSL */

    /* Retransmission / ACK-delay / PTO timer. Re-armed after every
     * read+drain cycle from ngtcp2_conn_get_expiry. Non-owning back-ref
     * to the same timer_cb that is registered on it — we keep it so
     * teardown can pull the callback before dispose. */
    zend_async_event_t          *timer;
    zend_async_event_callback_t *timer_cb;

    /* Deadline (ngtcp2 expiry, hrtime ns) the timer was last armed for.
     * timer_fire_cb subtracts it from the actual fire time to measure how
     * late the reactor serviced this connection's ACK/PTO — the per-conn
     * view of reactor stall. 0 = no timer armed. */
    uint64_t                     timer_expiry_ns;

    /* Intrusive list link. The listener tracks connections through this
     * chain instead of the DCID hashtable because the latter may carry
     * the same connection under multiple keys (server SCID + client
     * original DCID), and iterating the map would free the struct twice.
     * The hashtable stays non-owning; this list is the ownership edge. */
    http3_connection_t          *next;

    /* Deferred-output dirty-list link. The read path marks the
     * conn via http3_listener_mark_flush instead of draining per datagram;
     * the listener flushes the whole list once per recvmmsg tick, so a
     * burst of N datagrams for one conn coalesces into one drain (one GSO
     * sendmsg) instead of N. in_dirty guards against double-linking. */
    http3_connection_t          *dirty_next;
    bool                         in_dirty;

    /* Back-pointer to the owning listener. Used by ngtcp2 callbacks that
     * need to emit packets or update listener-level counters. Non-owning
     * reference — the listener outlives the connection. */
    http3_listener_t            *listener;

    /* Cached pointers into the owning server's hot-path counters / view
     * slices. Set at connection creation in http3_connection_dispatch
     * (or to the dummy / default fallbacks for an orphaned conn). Always
     * non-NULL — inline counter helpers in php_http_server.h bump these
     * directly with no NULL check. */
    http_server_counters_t      *counters;
    const http_server_view_t    *view;
    struct http_log_state       *log_state;

    /* Lifecycle flag. Set when the connection is transitioning out —
     * either closing handshake or we received an error. Prevents recv
     * callback from re-entering a dying conn. */
    bool closed;

    /* Set once http3_connection_emit_close has been called so teardown
     * does not emit a second CONNECTION_CLOSE on the same conn. */
    bool sent_connection_close;

    /* Migration-storm guard. A client that NAT-rebinds faster than
     * its path can validate (RFC 9000 §9.3 lets the server decline migration)
     * wedges ngtcp2 path validation — responses chase a stale path while the
     * live path only gets PTO probes. Count migrations in a sliding window;
     * past the cap we set migration_storm and shed the conn (graceful close to
     * the live peer) on the next flush instead of spinning probes for seconds.
     * Also hardens against a deliberate migration-flood DoS. */
    uint64_t migrate_window_start_ns;
    uint32_t migrate_count;
    bool     migration_storm;

    /* Reactor/worker dispatch home. The worker slot this connection's
     * requests stick to — one of this reactor's owned workers (reactor-paired pool).
     * -1 until the first dispatch homes it; re-homed if that worker dies. A busy
     * home spills individual requests elsewhere without changing this. Reactor-thread
     * only (the owning reactor dispatches every stream), so no atomics. */
    int worker_slot;

    /* Graceful drain state (mirror of the H1/H2 http_connection_t
     * fields). Pre-stamped at accept; consumed by the H3 commit path
     * which calls http_server_drain_evaluate and, on positive
     * result, submits an HTTP/3 GOAWAY via nghttp3_conn_shutdown. */
    bool      drain_pending;
    bool      drain_submitted;
    uint64_t  drain_epoch_seen;
    uint64_t  drain_not_before_ns;
    uint64_t  created_at_ns;

    /* Intrusive list of live streams (head). Linked at h3_begin_headers_cb,
     * unlinked at the final http3_stream_release. http3_connection_free
     * walks this list before nghttp3_conn_del to force-release any
     * stream nghttp3 would otherwise orphan at teardown. */
    http3_stream_t *streams_head;
};

/* Process an incoming QUIC packet:
 *
 *   - Route short-header packets to the matching connection by DCID
 *     (the DCID on wire matches our locally chosen SCID for this conn).
 *   - On long-header INITIAL with an unknown DCID, create a new
 *     connection via ngtcp2_conn_server_new.
 *
 * Returns true if the packet was recognised (whether or not it
 * advanced state), false if it was garbage that the listener should
 * count as parse_errors. Updates listener stats. */
bool http3_connection_dispatch(
    http3_listener_t *listener,
    const uint8_t *data, size_t datalen, uint8_t ecn,
    const struct sockaddr *peer, socklen_t peer_len);

/* Tear down a single connection: calls ngtcp2_conn_del, frees the
 * struct. Safe to call once per connection. */
void http3_connection_free(http3_connection_t *conn);

/* Unhook a connection from the listener (intrusive list +
 * conn_map under any registered key) AND free it. Used by the post-IO
 * terminal-state reaper and by future drain paths. Listener teardown
 * still walks conn_list directly with http3_connection_free since the
 * listener itself is going away. Safe to call exactly once per conn. */
void http3_connection_reap(http3_connection_t *conn);

/* Register a server-issued alternate CID (from get_new_connection_id_cb) in
 * the listener conn_map AND record it on the connection so it can be
 * unregistered at teardown. No-op on the (astronomically rare) byte-for-byte
 * collision with an existing key: the CID is then NOT recorded, so reap never
 * evicts a key it does not own. */
void http3_connection_register_issued_cid(
    http3_connection_t *c, const uint8_t *cid, size_t cidlen);

/* Unregister one issued CID (from remove_connection_id_cb on RETIRE). Deletes
 * the conn_map key only if this connection actually owns it. */
void http3_connection_unregister_issued_cid(
    http3_connection_t *c, const uint8_t *cid, size_t cidlen);

/* Delete every still-registered issued CID from the conn_map at teardown.
 * Called from http3_listener_remove_connection before the conn is freed. */
void http3_connection_unregister_all_issued_cids(http3_connection_t *c);

/* Flush one connection's pending ngtcp2 output and settle its lifecycle:
 * drain_out, then reap-or-arm-timer via check_terminal. Defined in
 * http3_io.c next to drain_out. The caller MUST NOT touch `conn` after
 * this returns — it may have been reaped. */
void http3_connection_flush(http3_connection_t *conn);

#endif /* HTTP3_CONNECTION_H */
