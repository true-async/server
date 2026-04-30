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
    zend_string    *remote_address;   /* lazily computed; owned; NULL until first read */
    bool            closed;           /* close() has been called or peer CLOSE seen */
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
    bool               committed;     /* upgrade dispatched — further mutation throws */
    zend_object        std;
} websocket_upgrade_object;

/* Pointer-arithmetic helpers — same pattern as http_response_from_obj. */
static zend_always_inline websocket_object *
websocket_from_obj(zend_object *obj) {
    return (websocket_object *)((char *)obj - XtOffsetOf(websocket_object, std));
}
static zend_always_inline websocket_message_object *
websocket_message_from_obj(zend_object *obj) {
    return (websocket_message_object *)((char *)obj - XtOffsetOf(websocket_message_object, std));
}
static zend_always_inline websocket_upgrade_object *
websocket_upgrade_from_obj(zend_object *obj) {
    return (websocket_upgrade_object *)((char *)obj - XtOffsetOf(websocket_upgrade_object, std));
}
#define Z_WEBSOCKET_P(zv)         websocket_from_obj(Z_OBJ_P(zv))
#define Z_WEBSOCKET_MESSAGE_P(zv) websocket_message_from_obj(Z_OBJ_P(zv))
#define Z_WEBSOCKET_UPGRADE_P(zv) websocket_upgrade_from_obj(Z_OBJ_P(zv))

/*
 * Factory: build a freshly-initialised PHP WebSocket object backed
 * by the given session. The returned object's refcount is 1 — the
 * caller is responsible for placing it in the zval that goes to the
 * handler. Subsequent method calls reach the session via
 * Z_WEBSOCKET_P(zv)->session.
 */
zend_object *websocket_object_create_for_session(ws_session_t *session);

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
