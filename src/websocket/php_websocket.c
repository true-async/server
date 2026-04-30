/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include "php.h"
#include "zend_exceptions.h"
#include "zend_enum.h"
#include "Zend/zend_async_API.h"
#include "websocket/php_websocket.h"
#include "websocket/ws_session.h"

/*
 * SCAFFOLD ONLY.
 *
 * This commit registers every WebSocket-related class and enum so the
 * public PHP API surface is visible at MINIT and reflectable from
 * userland. Method bodies throw "not implemented" for everything that
 * actually does work — recv / send / close / etc. land in dedicated
 * follow-up commits per docs/PLAN_WEBSOCKET.md §8.
 *
 * Splitting registration from behaviour makes each subsequent commit a
 * pure body replacement against a fixed signature, which is the
 * easiest possible review unit for security-sensitive code.
 */

zend_class_entry *websocket_ce                          = NULL;
zend_class_entry *websocket_message_ce                  = NULL;
zend_class_entry *websocket_upgrade_ce                  = NULL;
zend_class_entry *websocket_close_code_ce               = NULL;
zend_class_entry *websocket_exception_ce                = NULL;
zend_class_entry *websocket_closed_exception_ce         = NULL;
zend_class_entry *websocket_backpressure_exception_ce   = NULL;
zend_class_entry *websocket_concurrent_read_exception_ce = NULL;

/* http_server_exception_ce — base of the WebSocketException hierarchy.
 * Defined in http_server_exceptions.c, registered before us at MINIT. */
extern zend_class_entry *http_server_exception_ce;

/* Object handlers — one per concrete class. Initialised at MINIT by
 * memcpy'ing std_object_handlers and overriding offset / free_obj.
 * Same pattern as http_response_handlers in http_response.c. */
static zend_object_handlers websocket_handlers;
static zend_object_handlers websocket_message_handlers;
static zend_object_handlers websocket_upgrade_handlers;

/* {{{ create / free for WebSocket
 *
 * The default-constructed object has no session — the only legitimate
 * way to get a usable WebSocket is via the internal factory called
 * from the handshake path. Userland `new` is blocked by the private
 * constructor, but the create_object handler still has to be wired
 * because PHP calls it before the constructor visibility check.
 */
static zend_object *websocket_create(zend_class_entry *ce)
{
    websocket_object *obj = zend_object_alloc(sizeof(*obj), ce);

    obj->session        = NULL;
    obj->subprotocol    = NULL;
    obj->remote_address = NULL;
    obj->closed         = true;   /* default-closed; the factory clears this */

    zend_object_std_init(&obj->std, ce);
    object_properties_init(&obj->std, ce);
    obj->std.handlers = &websocket_handlers;

    return &obj->std;
}

static void websocket_free(zend_object *obj)
{
    websocket_object *w = websocket_from_obj(obj);

    /* Session is borrowed — the connection layer owns it and may
     * have already torn it down, in which case w->session is NULL.
     * We never call ws_session_destroy from here. */
    if (w->subprotocol) {
        zend_string_release(w->subprotocol);
    }
    if (w->remote_address) {
        zend_string_release(w->remote_address);
    }

    zend_object_std_dtor(&w->std);
}
/* }}} */

/* {{{ create / free for WebSocketMessage */
static zend_object *websocket_message_create(zend_class_entry *ce)
{
    websocket_message_object *obj = zend_object_alloc(sizeof(*obj), ce);

    obj->data   = NULL;
    obj->binary = false;

    zend_object_std_init(&obj->std, ce);
    object_properties_init(&obj->std, ce);
    obj->std.handlers = &websocket_message_handlers;

    return &obj->std;
}

static void websocket_message_free(zend_object *obj)
{
    websocket_message_object *m = websocket_message_from_obj(obj);
    if (m->data) {
        zend_string_release(m->data);
    }
    zend_object_std_dtor(&m->std);
}
/* }}} */

/* {{{ create / free for WebSocketUpgrade */
static zend_object *websocket_upgrade_create(zend_class_entry *ce)
{
    websocket_upgrade_object *obj = zend_object_alloc(sizeof(*obj), ce);

    obj->conn          = NULL;
    obj->req           = NULL;
    obj->subprotocol   = NULL;
    obj->reject_status = 0;
    obj->reject_reason = NULL;
    obj->committed     = false;

    zend_object_std_init(&obj->std, ce);
    object_properties_init(&obj->std, ce);
    obj->std.handlers = &websocket_upgrade_handlers;

    return &obj->std;
}

static void websocket_upgrade_free(zend_object *obj)
{
    websocket_upgrade_object *u = websocket_upgrade_from_obj(obj);
    if (u->subprotocol) {
        zend_string_release(u->subprotocol);
    }
    if (u->reject_reason) {
        zend_string_release(u->reject_reason);
    }
    zend_object_std_dtor(&u->std);
}
/* }}} */

/* {{{ Public factories — called from the handshake path */
zend_object *websocket_object_create_for_session(ws_session_t *session)
{
    zend_object *obj = websocket_create(websocket_ce);
    websocket_object *w = websocket_from_obj(obj);

    w->session = session;
    w->closed  = false;     /* fresh session — open until close() / peer FIN */

    return obj;
}

zend_object *websocket_message_object_create(zend_string *data, bool binary)
{
    zend_object *obj = websocket_message_create(websocket_message_ce);
    websocket_message_object *m = websocket_message_from_obj(obj);

    m->data   = data;       /* takes ownership — caller must have addref'd */
    m->binary = binary;

    /* Initialise the public readonly properties by writing directly
     * into the property slots — zend_update_property() goes through
     * the property handler which enforces the readonly invariant
     * even for internal callers, but this is the original
     * initialisation, so direct slot write is the documented
     * extension-author escape hatch (see ext/random for analogous
     * usage). Property layout matches stub declaration order:
     *   slot 0 = data (string)
     *   slot 1 = binary (bool)
     */
    zval *prop_data = OBJ_PROP_NUM(obj, 0);
    zval_ptr_dtor(prop_data);
    ZVAL_STR_COPY(prop_data, data);

    zval *prop_binary = OBJ_PROP_NUM(obj, 1);
    zval_ptr_dtor(prop_binary);
    ZVAL_BOOL(prop_binary, binary);

    return obj;
}

zend_object *websocket_upgrade_object_create(http_connection_t *conn,
                                             http_request_t *req)
{
    zend_object *obj = websocket_upgrade_create(websocket_upgrade_ce);
    websocket_upgrade_object *u = websocket_upgrade_from_obj(obj);

    u->conn = conn;
    u->req  = req;

    return obj;
}
/* }}} */

#include "../../stubs/WebSocketCloseCode.php_arginfo.h"
#include "../../stubs/WebSocketMessage.php_arginfo.h"
#include "../../stubs/WebSocketUpgrade.php_arginfo.h"
#include "../../stubs/WebSocket.php_arginfo.h"
#include "../../stubs/WebSocketExceptions.php_arginfo.h"

/* Common helper: every method body in this scaffold throws so that
 * accidental invocation surfaces a loud, actionable error instead of
 * silent success. Replaced one method at a time as real
 * implementations land. */
static void ws_throw_unimplemented(const char *what)
{
    zend_throw_exception_ex(websocket_exception_ce, 0,
        "WebSocket %s is not yet implemented in this build", what);
}

/* {{{ WebSocketMessage methods */
ZEND_METHOD(TrueAsync_WebSocketMessage, __construct)
{
    ZEND_PARSE_PARAMETERS_NONE();
    /* Constructor is private — instances are minted internally. The
     * body still exists because PHP requires every declared method to
     * have one; calling it directly via Reflection should just no-op. */
}
/* }}} */

/* {{{ WebSocketUpgrade methods */
ZEND_METHOD(TrueAsync_WebSocketUpgrade, __construct)
{
    ZEND_PARSE_PARAMETERS_NONE();
}

ZEND_METHOD(TrueAsync_WebSocketUpgrade, reject)
{
    zend_long status;
    zend_string *reason = NULL;

    ZEND_PARSE_PARAMETERS_START(1, 2)
        Z_PARAM_LONG(status)
        Z_PARAM_OPTIONAL
        Z_PARAM_STR(reason)
    ZEND_PARSE_PARAMETERS_END();

    (void)status; (void)reason;
    ws_throw_unimplemented("upgrade reject");
}

ZEND_METHOD(TrueAsync_WebSocketUpgrade, setSubprotocol)
{
    zend_string *name;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_STR(name)
    ZEND_PARSE_PARAMETERS_END();
    (void)name;
    ws_throw_unimplemented("setSubprotocol");
}

ZEND_METHOD(TrueAsync_WebSocketUpgrade, getOfferedSubprotocols)
{
    ZEND_PARSE_PARAMETERS_NONE();
    array_init(return_value);
}

ZEND_METHOD(TrueAsync_WebSocketUpgrade, getOfferedExtensions)
{
    ZEND_PARSE_PARAMETERS_NONE();
    array_init(return_value);
}
/* }}} */

/* {{{ WebSocket methods */
ZEND_METHOD(TrueAsync_WebSocket, __construct)
{
    ZEND_PARSE_PARAMETERS_NONE();
}

ZEND_METHOD(TrueAsync_WebSocket, recv)
{
    ZEND_PARSE_PARAMETERS_NONE();

    websocket_object *const w = Z_WEBSOCKET_P(ZEND_THIS);
    ws_session_t *const session = w->session;

    if (session == NULL || w->closed) {
        /* Already closed or never bound to a session — graceful end
         * of stream from the caller's perspective. */
        RETURN_NULL();
    }

    /* Single-reader enforcement (docs/PLAN_WEBSOCKET.md §6.9). The
     * wire is one byte stream; defining round-robin / fan-out
     * semantics for multiple readers has no real use case and is
     * easy to write race-prone code around, so we reject it loudly. */
    zend_coroutine_t *const me = ZEND_ASYNC_CURRENT_COROUTINE;
    if (session->recv_waiter != NULL && session->recv_waiter != me) {
        zend_throw_exception(websocket_concurrent_read_exception_ce,
            "Another coroutine is already suspended in WebSocket::recv() "
            "on this connection", 0);
        RETURN_THROWS();
    }

    while (1) {
        /* Fast path: a message is already in the FIFO. Pop it and
         * hand it back as a fresh WebSocketMessage. */
        ws_pending_message_t *node = ws_session_recv_pop(session);
        if (node != NULL) {
            session->recv_waiter = NULL;
            zend_object *msg = websocket_message_object_create(node->data,
                                                               node->binary);
            efree(node);  /* data ownership transferred to the message obj */
            RETURN_OBJ(msg);
        }

        /* Drained — was the connection closed while we waited? */
        if (session->peer_closed) {
            session->recv_waiter = NULL;
            RETURN_NULL();
        }

        /* Need to suspend until on_msg_recv_callback (or peer FIN)
         * notifies the recv_event. Lazy-create the trigger event
         * so connections that never actually call recv() pay no
         * setup cost. */
        if (session->recv_event == NULL) {
            session->recv_event = ZEND_ASYNC_NEW_TRIGGER_EVENT();
            if (session->recv_event == NULL) {
                session->recv_waiter = NULL;
                RETURN_NULL();
            }
        }

        if (ZEND_ASYNC_WAKER_NEW(me) == NULL) {
            session->recv_waiter = NULL;
            return;
        }
        session->recv_waiter = me;

        zend_async_resume_when(me, &session->recv_event->base, false,
                               zend_async_waker_callback_resolve, NULL);

        ZEND_ASYNC_SUSPEND();
        zend_async_waker_clean(me);

        if (EG(exception) != NULL) {
            session->recv_waiter = NULL;
            return;
        }

        /* Loop and re-check the FIFO — woken either by a message or
         * by mark_peer_closed. */
    }
}

/* {{{ ws_do_send — internal helper for both send() and sendBinary()
 *
 * 1. wslay_event_queue_msg — copies the payload into wslay's outbound
 *    queue (multi-producer-safe; wslay's queue is just a FIFO of
 *    message structs, mutated only on the single-threaded scheduler).
 * 2. Flusher discipline (PLAN_WEBSOCKET.md §2.4): if no other
 *    coroutine currently holds the flusher role, become one and
 *    drive wslay_event_send. Otherwise just enqueue and return —
 *    the active flusher will eventually pick our bytes up.
 *
 * Backpressure (queue size cap, suspension on overflow, escape via
 * WebSocketBackpressureException) is the next commit; for now the
 * queue is unbounded.
 */
static void ws_do_send(zval *zv_this, zend_string *payload, uint8_t opcode)
{
    websocket_object *const w = Z_WEBSOCKET_P(zv_this);
    if (w->session == NULL || w->closed) {
        zend_throw_exception_ex(websocket_closed_exception_ce, 0,
            "Cannot send on a closed WebSocket");
        return;
    }
    ws_session_t *const s = w->session;

    const struct wslay_event_msg msg = {
        .opcode     = opcode,
        .msg        = (const uint8_t *)ZSTR_VAL(payload),
        .msg_length = ZSTR_LEN(payload),
    };
    if (wslay_event_queue_msg(s->ctx, &msg) != 0) {
        zend_throw_exception_ex(websocket_exception_ce, 0,
            "WebSocket queue_msg failed (out of memory or session closed)");
        return;
    }

    if (s->flushing) {
        /* Another coroutine already drives the flusher; it'll pick up
         * the message we just enqueued. */
        return;
    }

    s->flushing = 1;
    const int rc = wslay_event_send(s->ctx);
    s->flushing = 0;

    if (rc != 0) {
        /* Sticky write error or session-internal failure. The next
         * recv()/send() will see write_error and route to a closed
         * exception; we don't tear the connection down from here
         * because the read-side teardown owns that path. */
        zend_throw_exception_ex(websocket_closed_exception_ce, 0,
            "WebSocket send failed (peer closed or write error)");
    }
}
/* }}} */

ZEND_METHOD(TrueAsync_WebSocket, send)
{
    zend_string *text;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_STR(text)
    ZEND_PARSE_PARAMETERS_END();
    ws_do_send(ZEND_THIS, text, WSLAY_TEXT_FRAME);
}

ZEND_METHOD(TrueAsync_WebSocket, sendBinary)
{
    zend_string *data;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_STR(data)
    ZEND_PARSE_PARAMETERS_END();
    ws_do_send(ZEND_THIS, data, WSLAY_BINARY_FRAME);
}

ZEND_METHOD(TrueAsync_WebSocket, ping)
{
    zend_string *payload = NULL;
    ZEND_PARSE_PARAMETERS_START(0, 1)
        Z_PARAM_OPTIONAL
        Z_PARAM_STR(payload)
    ZEND_PARSE_PARAMETERS_END();
    (void)payload;
    ws_throw_unimplemented("ping");
}

ZEND_METHOD(TrueAsync_WebSocket, close)
{
    zval *code = NULL;
    zend_string *reason = NULL;

    ZEND_PARSE_PARAMETERS_START(0, 2)
        Z_PARAM_OPTIONAL
        Z_PARAM_ZVAL(code)
        Z_PARAM_STR(reason)
    ZEND_PARSE_PARAMETERS_END();

    (void)code; (void)reason;
    ws_throw_unimplemented("close");
}

ZEND_METHOD(TrueAsync_WebSocket, isClosed)
{
    ZEND_PARSE_PARAMETERS_NONE();
    websocket_object *w = Z_WEBSOCKET_P(ZEND_THIS);
    /* `closed` is true after close() lands or peer CLOSE is processed,
     * and also true on a default-constructed object that was never
     * bound to a session — which is what the private constructor +
     * Reflection-instantiation case looks like. */
    RETURN_BOOL(w->closed || w->session == NULL);
}

ZEND_METHOD(TrueAsync_WebSocket, getSubprotocol)
{
    ZEND_PARSE_PARAMETERS_NONE();
    websocket_object *w = Z_WEBSOCKET_P(ZEND_THIS);
    if (w->subprotocol) {
        RETURN_STR_COPY(w->subprotocol);
    }
    RETURN_NULL();
}

ZEND_METHOD(TrueAsync_WebSocket, getRemoteAddress)
{
    ZEND_PARSE_PARAMETERS_NONE();
    websocket_object *w = Z_WEBSOCKET_P(ZEND_THIS);
    if (w->remote_address) {
        RETURN_STR_COPY(w->remote_address);
    }
    /* Lazy resolution from the connection lands when handshake wiring
     * commits — for now an unbound or not-yet-resolved object reports
     * the empty string rather than NULL so the return-type contract
     * (`: string`) is honoured. */
    RETURN_EMPTY_STRING();
}
/* }}} */

void ws_php_classes_register(void)
{
    /* Order: the close-code enum and the value object first because
     * other classes reference them by name in their arginfo. */
    websocket_close_code_ce = register_class_TrueAsync_WebSocketCloseCode();
    websocket_message_ce    = register_class_TrueAsync_WebSocketMessage();
    websocket_upgrade_ce    = register_class_TrueAsync_WebSocketUpgrade();
    websocket_ce            = register_class_TrueAsync_WebSocket();

    /* Wire object handlers. Same pattern as http_response_handlers —
     * memcpy std defaults, then override offset (so PHP knows where
     * the embedded zend_object sits inside our struct) and free_obj
     * (so our owned strings get released). clone_obj = NULL because
     * none of these are meaningfully cloneable. */
    memcpy(&websocket_handlers, &std_object_handlers, sizeof(zend_object_handlers));
    websocket_handlers.offset    = XtOffsetOf(websocket_object, std);
    websocket_handlers.free_obj  = websocket_free;
    websocket_handlers.clone_obj = NULL;
    websocket_ce->create_object  = websocket_create;

    memcpy(&websocket_message_handlers, &std_object_handlers, sizeof(zend_object_handlers));
    websocket_message_handlers.offset    = XtOffsetOf(websocket_message_object, std);
    websocket_message_handlers.free_obj  = websocket_message_free;
    websocket_message_handlers.clone_obj = NULL;
    websocket_message_ce->create_object  = websocket_message_create;

    memcpy(&websocket_upgrade_handlers, &std_object_handlers, sizeof(zend_object_handlers));
    websocket_upgrade_handlers.offset    = XtOffsetOf(websocket_upgrade_object, std);
    websocket_upgrade_handlers.free_obj  = websocket_upgrade_free;
    websocket_upgrade_handlers.clone_obj = NULL;
    websocket_upgrade_ce->create_object  = websocket_upgrade_create;

    /* Exceptions inherit from the project base. Register the WS base
     * first so the leaves can chain to it. */
    websocket_exception_ce =
        register_class_TrueAsync_WebSocketException(http_server_exception_ce);
    websocket_closed_exception_ce =
        register_class_TrueAsync_WebSocketClosedException(websocket_exception_ce);
    websocket_backpressure_exception_ce =
        register_class_TrueAsync_WebSocketBackpressureException(websocket_exception_ce);
    websocket_concurrent_read_exception_ce =
        register_class_TrueAsync_WebSocketConcurrentReadException(websocket_exception_ce);
}
