/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

#ifndef PHP_WEBSOCKET_H
#define PHP_WEBSOCKET_H

#include "php.h"
#include "php_http_server.h"
#include "websocket/ws_session.h"

typedef struct http_request_t http_request_t;

/* Class entries — populated by ws_php_classes_register() at MINIT
 * and read from anywhere that constructs / type-checks WS objects. */
extern zend_class_entry *websocket_ce;
extern zend_class_entry *websocket_message_ce;
extern zend_class_entry *websocket_upgrade_ce;
extern zend_class_entry *websocket_close_code_ce;

/* Exception hierarchy. WebSocketException extends HttpServerException
 * (the project's base) so generic `catch (HttpServerException $e)`
 * keeps catching WS errors. */
extern zend_class_entry *websocket_exception_ce;
extern zend_class_entry *websocket_closed_exception_ce;
extern zend_class_entry *websocket_backpressure_exception_ce;
extern zend_class_entry *websocket_concurrent_read_exception_ce;

/*
 * Register all five WebSocket-related classes (+ the close-code enum
 * + the four exception classes). Must be called from MINIT after the
 * project's HttpServerException hierarchy is registered, since
 * WebSocketException inherits from it.
 *
 * No-op when --disable-websocket; the symbol still exists so the call
 * site in http_server.c does not need its own #ifdef.
 */
void ws_php_classes_register(void);

/*
 * WebSocket PHP object backing.
 *
 * Holds the borrowed session pointer plus a small slice of state that
 * the public methods (recv / send / close / getRemoteAddress / etc.)
 * read on every call. The session itself owns the wslay context; the
 * PHP object owns nothing the connection layer needs to reach into.
 *
 * `session` may be NULL after the connection is torn down — methods
 * must check before dereferencing. Ownership: the session pointer is
 * borrowed from the connection, which outlives every PHP reference
 * to this object via handler_refcount (see http_connection.h).
 */
typedef struct {
    ws_session_t   *session;          /* borrowed; cleared on connection teardown */
    zend_string    *subprotocol;      /* selected subprotocol (NULL = none); owned */
    /* Peer, snapshotted at upgrade while conn is guaranteed alive. Bare IP —
     * the port is separate, never glued into the string (see http_sockaddr_ip).
     * NULL / 0 for a Unix-socket listener. */
    zend_string    *remote_address;
    uint16_t        remote_port;
    bool            closed;           /* close() has been called or peer CLOSE seen */

    /* Pre-commit upgrade state. The handler runs BEFORE the 101
     * response is sent so it can reject / set subprotocol via the
     * WebSocketUpgrade handle. The first WS I/O method (recv / send
     * / close) auto-commits: ws_commit_upgrade flushes 101, installs
     * the WS strategy, creates the wslay session. After commit the
     * fields below are inert.
     *
     * `accept_value` is computed at try_upgrade time (no I/O risk —
     * SHA1+base64 is pure CPU) so the commit step does not need the
     * raw client key any more.
     *
     * `upgrade_zv` holds a strong ref to the WebSocketUpgrade object
     * so the handler's early WebSocketUpgrade calls (reject /
     * setSubprotocol) write into the same struct that commit reads.
     */
    bool            committed;        /* 101 sent + strategy installed */
    char            accept_value[28]; /* WS_ACCEPT_LEN; pre-computed */
    zval            upgrade_zv;       /* IS_OBJECT WebSocketUpgrade; cleared on dispose */
    http_connection_t *conn;          /* borrowed; needed for commit-time I/O */
    struct http2_stream_t *h2_stream; /* RFC 8441: set when this WS lives in
                                       * an H2 stream; NULL for H1 / wss */
    /* foreach cursor (Iterator): current message + key. iter_current is a
     * WebSocketMessage while iterating, IS_NULL at end-of-stream. */
    zval            iter_current;
    zend_long       iter_key;
    zend_object     std;
} websocket_object;

/*
 * WebSocketMessage value object — readonly text/binary slice handed
 * to the PHP handler from recv().
 */
typedef struct {
    zend_string    *data;             /* owned */
    bool            binary;           /* false = text frame (UTF-8 valid), true = binary */
    zend_object     std;
} websocket_message_object;

/*
 * WebSocketUpgrade pre-handshake handle. Lives only between the
 * handler's first instruction and either reject() / handler return.
 * `conn` and `req` are borrowed.
 */
typedef struct {
    http_connection_t *conn;          /* borrowed */
    http_request_t    *req;           /* borrowed */
    zend_string       *subprotocol;   /* selected by setSubprotocol; owned */
    int                reject_status; /* 0 = accept, 4xx/5xx = reject */
    zend_string       *reject_reason; /* owned; NULL when not rejecting */
    zend_object       *ws;            /* borrowed back-pointer to the WebSocket */
    bool               committed;     /* upgrade dispatched — further mutation throws */
    zend_object        std;
} websocket_upgrade_object;

/* Pointer-arithmetic helpers — same pattern as http_response_from_obj. */
static zend_always_inline websocket_object *
websocket_from_obj(zend_object *obj) {
    return (websocket_object *)((char *)obj - offsetof(websocket_object, std));
}
static zend_always_inline websocket_message_object *
websocket_message_from_obj(zend_object *obj) {
    return (websocket_message_object *)((char *)obj - offsetof(websocket_message_object, std));
}
static zend_always_inline websocket_upgrade_object *
websocket_upgrade_from_obj(zend_object *obj) {
    return (websocket_upgrade_object *)((char *)obj - offsetof(websocket_upgrade_object, std));
}
#define Z_WEBSOCKET_P(zv)         websocket_from_obj(Z_OBJ_P(zv))
#define Z_WEBSOCKET_MESSAGE_P(zv) websocket_message_from_obj(Z_OBJ_P(zv))
#define Z_WEBSOCKET_UPGRADE_P(zv) websocket_upgrade_from_obj(Z_OBJ_P(zv))

/*
 * Factory: build a freshly-initialised PHP WebSocket object in the
 * pre-commit state. `accept_value` is the 28-char Sec-WebSocket-Accept
 * computed at validation time. The session pointer stays NULL until
 * ws_commit_upgrade() flushes the 101 and installs the WS strategy.
 */
zend_object *websocket_object_create_pre_commit(http_connection_t *conn,
                                                const char *accept_value);

/*
 * Drive the deferred upgrade through to completion. Invoked from the
 * prologue of every WS I/O method (recv / send / close) when
 * !w->committed, and as the auto-commit step in handler dispose.
 *
 * Returns true on a successful 101 flush + strategy install (caller
 * proceeds). Returns false when the WebSocketUpgrade was rejected —
 * caller should abort (an exception is set). Suspends the calling
 * coroutine on the 101 socket write.
 */
bool ws_commit_upgrade(websocket_object *w, bool install_session);

/* HTTP/2 (RFC 8441) accept: commit a streaming 200 on the stream, create
 * the per-stream wslay session bound to the H2 DATA transport, and set
 * both stream->ws_session and w->session. Defined in http2_strategy.c
 * (only when HTTP/2 is compiled in). Returns false on failure. */
struct http2_stream_t;
bool http2_ws_accept(struct http2_stream_t *stream, websocket_object *w,
                     const char *subprotocol, int deflate_bits);

/*
 * Factory: build a WebSocketMessage from the assembled payload
 * delivered by ws_session_on_msg_recv_callback. Takes ownership of
 * `data` (no addref by the factory — pass an addref'd or fresh string).
 */
zend_object *websocket_message_object_create(zend_string *data, bool binary);

/*
 * Factory: build a WebSocketUpgrade for the in-progress handshake.
 * `conn` and `req` are borrowed (must outlive the object).
 */
zend_object *websocket_upgrade_object_create(http_connection_t *conn,
                                             http_request_t *req);

#endif /* PHP_WEBSOCKET_H */
