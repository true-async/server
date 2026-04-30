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

    /* Peer address (latest observed). Updated each time we successfully
     * read a packet from a new path — but we don't implement migration
     * yet, so this is effectively the initial peer. */
    struct sockaddr_storage peer;
    socklen_t               peer_len;

    /* Opaque ngtcp2_conn pointer. void* to keep ngtcp2 types out of this
     * header — the concrete type is ngtcp2_conn *. Accessed via
     * http3_connection_ngtcp2(). */
    void *ngtcp2_conn;

    /* Opaque nghttp3_conn (HTTP/3 framing layer). NULL until
     * the TLS handshake completes; created in handshake_completed_cb so
     * that it is only ever attached to a verified-h3 peer. */
    void *nghttp3_conn;

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

    /* Intrusive list link. The listener tracks connections through this
     * chain instead of the DCID hashtable because the latter may carry
     * the same connection under multiple keys (server SCID + client
     * original DCID), and iterating the map would free the struct twice.
     * The hashtable stays non-owning; this list is the ownership edge. */
    http3_connection_t          *next;

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

#endif /* HTTP3_CONNECTION_H */
