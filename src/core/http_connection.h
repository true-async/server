/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

#ifndef HTTP_CONNECTION_H
#define HTTP_CONNECTION_H

#include "php.h"
#include "main/php_network.h"   /* php_socket_t, SOCK_ERR, closesocket */
#include "Zend/zend_async_API.h"
#include "php_http_server.h"
#include "tls_layer.h"

#ifdef HAVE_OPENSSL
#include <openssl/bio.h>
#endif

#include <stdint.h>

/* Per-connection plaintext ring buffer feeding the TLS flusher.
 * Producers (handler coroutines) push plaintext via BIO_write; the
 * flusher drains it through SSL_write. Sized for one MAX_FRAME_SIZE
 * H2 frame (16 KiB) plus one in-flight TLS record (16 KiB); a full
 * ring triggers backpressure via tls_drain_event. */
#define HTTP_TLS_PLAINTEXT_RING_BYTES (32 * 1024)

typedef struct http1_parser_t http1_parser_t;
typedef struct http_request_t http_request_t;
typedef struct _http_protocol_strategy_t http_protocol_strategy_t;
typedef struct _http_connection_read_cb http_connection_read_cb_t;
#ifdef HAVE_OPENSSL
typedef struct _tls_fsm_send_cb tls_fsm_send_cb_t;
#endif

typedef enum {
    /* Pre-request. For TLS connections the initial state is
     * CONN_STATE_TLS_HANDSHAKE; once handshake completes, the connection
     * transitions to CONN_STATE_READING_HEADERS and the rest of the
     * pipeline is protocol-agnostic. */
    CONN_STATE_TLS_HANDSHAKE,
    CONN_STATE_READING_HEADERS,
    CONN_STATE_READING_BODY,
    CONN_STATE_PROCESSING,
    CONN_STATE_SENDING,
    CONN_STATE_KEEPALIVE_WAIT,
    CONN_STATE_CLOSING
} http_connection_state_t;

/* Connection structure
 * Note: http_connection_t is forward-declared in php_http_server.h
 * as 'typedef struct _http_connection_t http_connection_t;'
 *
 * Field order is deliberate: grouped by alignment (pointers, size_t,
 * socket, 32-bit, small) to minimise padding on both 64-bit POSIX and
 * Windows. Keep bool flags clustered at the end.
 */
struct _http_connection_t {
    /* Pointers (8 bytes on 64-bit) */
    http_protocol_strategy_t  *strategy;
    http1_parser_t            *parser;
    zend_fcall_t              *handler;
    char                      *read_buffer;
    zend_async_scope_t        *scope;
    zend_async_io_t           *io;         /* TrueAsync TCP IO handle — owns the accepted socket */
    http_connection_read_cb_t *read_cb;    /* Persistent read callback attached to io->event */

    /* Owning server. NULL when the connection runs unsupervised (tests).
     * http_connection.c calls http_server_on_request_sample /
     * http_server_on_connection_close directly through this pointer.
     * Timing fields live on http_request_t (sojourn is per-request, not
     * per-TCP-socket — keep-alive + HTTP/2 multiplex many requests per
     * connection). */
    http_server_object        *server;

    /* Cached pointers into the server's embedded counters slice + view.
     * Set at create time to either &server->counters/view (when supervised)
     * or &http_server_counters_dummy / &http_server_view_default (when not).
     * Always non-NULL after http_connection_create returns — hot-path
     * callers bump fields directly via the inline helpers in
     * php_http_server.h, no NULL check, no function call.
     * http_server_free re-points these at the dummies before freeing the
     * server struct so any post-free counter bump becomes a write to the
     * static garbage object instead of a UAF. */
    http_server_counters_t    *counters;
    const http_server_view_t  *view;

    /* Sink for log emits originating from this connection. Set at
     * create time to &server->log_state (or &http_log_state_default
     * when unsupervised). http_server_free re-points back to default
     * before freeing the server, so post-free emits drop silently
     * instead of UAFing. Same discipline as counters/view. */
    struct http_log_state     *log_state;

    /* Pointer to the request currently being handled. NULL between
     * requests. Used by the handler coroutine to read/write timing
     * fields on the live request without reaching through
     * request_zv. For HTTP/2 this becomes a HashTable<stream_id>. */
    http_request_t            *current_request;

    /* Per-connection TLS session. NULL on plaintext listeners; non-NULL
     * means every byte on the wire goes through feed/drain_ciphertext.
     * The session holds an SSL* whose rbio/wbio is a BIO pair — OpenSSL
     * owns one half, we own the other and shuttle between the socket
     * and the user of the read/write buffers. */
#ifdef HAVE_OPENSSL
    tls_session_t                 *tls;

    /* Plaintext queue feeding the cooperative flusher. Active only while
     * conn->tls != NULL. Producers (handler coroutines) push plaintext
     * into tls_plaintext_bio via BIO_write — a pure memory op, no SSL
     * state. The first producer that finds tls_flushing == false becomes
     * the flusher: it drains tls_plaintext_bio_app via SSL_write and
     * ships the ciphertext through the suspending send_raw path. Later
     * producers enqueue and return — they do not wait for their bytes
     * to hit the wire; the flusher picks them up on its next loop. */
    BIO                           *tls_plaintext_bio;       /* producer side (BIO_write here) */
    BIO                           *tls_plaintext_bio_app;   /* flusher side (BIO_nread here) */
    zend_async_trigger_event_t    *tls_drain_event;         /* fires when ring gains space */

    /* Read FSM async-write slot. The TLS read FSM lives in event-loop
     * callback context (no coroutine, no suspension). Bytes it produces
     * via SSL_do_handshake / SSL_read (handshake, NewSessionTicket,
     * KeyUpdate, alerts, close_notify) are copied into a private
     * heap buffer and shipped via a non-blocking ZEND_ASYNC_IO_WRITE.
     * The persistent send-completion callback frees the buffer and
     * re-enters the FSM. Coordination with the producer flusher is via
     * tls_flushing: while the handler-side flusher owns the cipher
     * BIO, the FSM defers and lets that loop pick up its bytes. */
    tls_fsm_send_cb_t             *tls_fsm_send_cb;

    /* Bit-fields packed with the rest of the flag word at the end of
     * the struct to save padding. unsigned : 1 chosen over bool : 1 for
     * the broadest compiler portability; reads/writes use plain
     * bool/true/false via implicit conversion. */
    unsigned                       tls_flushing : 1;         /* producer-side flusher role held */
    unsigned                       tls_write_error : 1;      /* sticky SSL_write / socket failure */
    unsigned                       tls_awaiting_handler : 1; /* FSM paused until handler dispose */
#endif

    /* size_t fields (8 bytes on 64-bit) */
    size_t                   read_buffer_size;
    size_t                   read_buffer_len;

    /* 4-byte fields */
    http_connection_state_t  state;
    http_protocol_type_t     protocol_type;
    uint32_t                 read_timeout_ms;       /* milliseconds; 0 = disabled */
    uint32_t                 write_timeout_ms;      /* milliseconds; 0 = disabled */
    uint32_t                 keepalive_timeout_ms;  /* milliseconds; 0 = disabled */

    char                     http_version[8];

    /* Graceful drain state. Pull-model: no server-side connection list,
     * no walk. should_drain_now() reads these fields and decides
     * whether to append `Connection: close` / GOAWAY to the current
     * response. */
    uint64_t                 created_at_ns;          /* stamp on spawn — input for proactive age drain */
    uint64_t                 drain_not_before_ns;    /* effective drain time: proactive = age+jitter, reactive = now+spread_jitter */
    uint64_t                 drain_epoch_seen;       /* last server->drain_epoch_current this conn observed */

    /* Grace-timer slot (reserved for future implementation).
     * MVP deferral: with the default max_connection_age_grace_ns == 0
     * (infinite grace) no timer is armed anyway, so pathological-
     * client force-close relies on keepalive_timeout_ms / read_timeout_ms.
     * The config knob + server->connections_force_closed_total
     * counter are already wired so a later PR can fill this slot
     * without changing the public API. */
    void *grace_timer_reserved;

    /* Flags as 1-bit fields — all booleans pack into a single 32-bit
     * word together with write_timed_out below, eliminating ~16 bytes
     * of padding vs separate `bool` storage on x86_64. */
    unsigned                 protocol_detected : 1;
    unsigned                 keep_alive : 1;
    unsigned                 headers_complete : 1;
    unsigned                 body_complete : 1;
    unsigned                 request_ready : 1;     /* set by strategy->on_request_ready */
    unsigned                 drain_pending : 1;     /* decision: this conn should drain */
    unsigned                 drain_submitted : 1;   /* HTTP/2: GOAWAY already queued on this session */
    unsigned                 destroy_pending : 1;   /* destroy deferred — a handler coroutine is mid-dispose */

    /* Intrusive single-link node in http_server_object::conn_list. The
     * list lets http_server_free() walk all live connections and clear
     * their server back-pointer before the server struct is freed —
     * otherwise libuv's shutdown drain fires the per-conn read callback
     * after server is gone, and on_connection_close UAFs. NULL means
     * "not in any list" (e.g. server == NULL at create time, or already
     * unlinked by destroy). */
    http_connection_t       *next_conn;

    /* Number of handler coroutines currently holding a reference to
     * this connection (H2 multiplex: N concurrent handler coroutines
     * per TCP). Incremented at dispatch, decremented at dispose end.
     * While > 0, http_connection_destroy defers by setting
     * destroy_pending; the last handler's dispose finalises the free.
     * Prevents the classic async UAF where a read-callback-initiated
     * destroy races against a suspended handler's commit_stream_response
     * drain loop — observable as a SEGV on stale session pointers under
     * concurrent peer-FIN during dispose. */
    uint32_t                 handler_refcount;

    /* Per-connection write deadline timer, replacing the per-await
     * timer that async_io_req_await used to allocate per write.
     * Lazy-created on first send_raw entry, REARMed on subsequent
     * entries, stopped on every successful exit. Fires → flips
     * write_timed_out and closes io so the suspended writer's req
     * surfaces an exception. Persistent across keep-alive requests. */
    zend_async_event_t            *write_timer;       /* zend_async_timer_event_t* */
    zend_async_event_callback_t   *write_timer_cb;
    unsigned                       write_timed_out : 1;
};

/* Per-request state for the HTTP/1 handler coroutine. Lives on the
 * coroutine (extended_data), not on the connection — so a pipelined
 * request N+1 dispatched while N's dispose is still draining cannot
 * clobber N's request/response zvals. Mirrors the H2 design where the
 * equivalent state lives on http2_stream_t.
 *
 * Allocated in http_connection_dispatch_request, freed in
 * http_handler_coroutine_dispose. The conn back-pointer is borrowed
 * (the connection outlives every in-flight handler thanks to
 * conn->handler_refcount). */
typedef struct {
    http_connection_t *conn;
    http_request_t    *request;            /* parsed request, owned by parser pool */
    zval               request_zv;
    zval               response_zv;
    bool               h1_stream_headers_sent;  /* status+headers already on wire (chunked) */
} http1_request_ctx_t;

/* Connection lifecycle */
http_connection_t *http_connection_create(php_socket_t socket_fd, zend_async_scope_t *parent_scope);
void http_connection_destroy(http_connection_t *conn);

/* Connection processing */
bool http_connection_send(http_connection_t *conn, const char *data, size_t len);
bool http_connection_send_error(http_connection_t *conn, int status_code, const char *message);

/* Build and emit the RFC-compliant 4xx response for a parser failure.
 * Reads parser->parse_error, maps to status + reason, writes through
 * http_connection_send (plaintext or TLS), bumps the parse-error
 * telemetry counter, and forces keep_alive=false so the dispose path
 * tears down after the write. Best-effort — returns false if the
 * socket is already dead. */
bool http_connection_emit_parse_error(http_connection_t *conn, http1_parser_t *parser);

/* Submit a ZEND_ASYNC_IO_READ; either process the sync completion
 * inline or register the persistent read callback on io->event. The
 * handler coroutine is spawned from on_request_ready and re-arms
 * reads itself for keep-alive. */
bool http_connection_read(http_connection_t *conn);

/* Helper to spawn connection coroutine.
 * server may be NULL — the connection will run without backpressure
 * reporting (useful for tests).
 * tls_ctx is passed only when this connection was accepted on a TLS
 * listener. When non-NULL and OpenSSL is compiled in, the connection
 * starts in CONN_STATE_TLS_HANDSHAKE and an event-driven FSM in the
 * read callback drives the handshake then the decrypt/feed/dispatch
 * loop. NULL means plaintext.
 * Returns true on success; on failure the fd is already closed and the
 * server is never notified. */
bool http_connection_spawn(php_socket_t client_fd, zend_async_scope_t *server_scope,
                           zend_fcall_t *handler,
                           uint32_t read_timeout_ms, uint32_t write_timeout_ms, uint32_t keepalive_timeout_ms,
                           http_server_object *server,
                           tls_context_t *tls_ctx);

#endif /* HTTP_CONNECTION_H */
