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
#include "websocket/ws_handshake.h"
#include "websocket/websocket_strategy.h"
#include "core/http_connection.h"
#include "core/http_protocol_strategy.h"
#include "http1/http_parser.h"   /* full http_request_t for u->req->headers */

#include <string.h>

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
    obj->committed      = false;
    obj->conn           = NULL;
    ZVAL_UNDEF(&obj->upgrade_zv);
    memset(obj->accept_value, 0, sizeof(obj->accept_value));

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
    if (Z_TYPE(w->upgrade_zv) != IS_UNDEF) {
        zval_ptr_dtor(&w->upgrade_zv);
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
zend_object *websocket_object_create_pre_commit(http_connection_t *conn,
                                                const char *accept_value)
{
    zend_object *obj = websocket_create(websocket_ce);
    websocket_object *w = websocket_from_obj(obj);

    w->session   = NULL;          /* created by commit_upgrade */
    w->closed    = false;         /* open from the handler's perspective */
    w->committed = false;         /* not yet — handler runs first */
    w->conn      = conn;
    memcpy(w->accept_value, accept_value, sizeof(w->accept_value));

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

/* {{{ ws_commit_upgrade
 *
 * Drive the deferred 101 Switching Protocols send and bring the WS
 * session online. Called from the prologue of every WS I/O method
 * (recv / send / close) when w->committed is false, and from the
 * handler dispose path when the handler exited without doing any
 * WS I/O at all.
 *
 * Resolves whatever subprotocol setSubprotocol() picked (read from
 * the WebSocketUpgrade object held in w->upgrade_zv), formats the
 * 101 response, sends it via http_connection_send (suspending — we
 * are by definition in a handler coroutine), then installs the WS
 * strategy and creates the wslay session bound to it.
 *
 * On reject (WebSocketUpgrade::reject was called BEFORE any WS I/O),
 * sets the appropriate exception and returns false. Caller propagates.
 */
bool ws_commit_upgrade(websocket_object *w)
{
    if (w->committed) {
        return true;
    }
    if (w->conn == NULL) {
        zend_throw_exception_ex(websocket_closed_exception_ce, 0,
            "WebSocket has no connection");
        return false;
    }

    /* If reject() was called early, surface a closed-exception and
     * leave it to dispose to actually emit the 4xx response. We do
     * not send the 4xx here — we are inside an I/O method invoked
     * by the handler, and the canonical exit is throw → unwind to
     * handler exit → dispose path. */
    if (Z_TYPE(w->upgrade_zv) == IS_OBJECT) {
        websocket_upgrade_object *u =
            websocket_upgrade_from_obj(Z_OBJ(w->upgrade_zv));
        if (u->reject_status != 0) {
            zend_throw_exception_ex(websocket_closed_exception_ce, 0,
                "WebSocket upgrade was rejected (%d)", u->reject_status);
            return false;
        }
    }

    /* Build the 101 with whatever subprotocol setSubprotocol picked
     * (NULL = none). Subprotocol comes from the upgrade-object slot
     * if a third-arg handler set it; otherwise the field stays NULL
     * and Sec-WebSocket-Protocol is omitted. */
    const char *subprotocol = NULL;
    if (Z_TYPE(w->upgrade_zv) == IS_OBJECT) {
        websocket_upgrade_object *u =
            websocket_upgrade_from_obj(Z_OBJ(w->upgrade_zv));
        if (u->subprotocol != NULL) {
            subprotocol = ZSTR_VAL(u->subprotocol);
            /* Mirror the selected subprotocol onto the WebSocket
             * object so getSubprotocol() can serve it without
             * reaching back through the upgrade handle (which may
             * be cleared). */
            w->subprotocol = zend_string_copy(u->subprotocol);
        }
    }

    zend_string *resp = ws_handshake_build_101_response(w->accept_value,
                                                        subprotocol);
    if (resp == NULL) {
        zend_throw_exception_ex(websocket_closed_exception_ce, 0,
            "WebSocket: out of memory building 101 response");
        return false;
    }

    /* Suspending send — we are in coroutine context (handler called
     * us) so this is the canonical use of http_connection_send. */
    bool ok = http_connection_send(w->conn,
                                   ZSTR_VAL(resp), ZSTR_LEN(resp));
    zend_string_release(resp);

    if (!ok) {
        zend_throw_exception_ex(websocket_closed_exception_ce, 0,
            "WebSocket: peer closed before 101 could be flushed");
        w->closed = true;
        return false;
    }

    /* 101 is on the wire. Install the WS strategy + create session.
     * After this, future read callbacks route bytes into ws_session
     * and the recv/send/close paths can do their thing. */
    http_connection_t *conn = w->conn;
    if (conn->strategy != NULL) {
        if (conn->strategy->cleanup) {
            conn->strategy->cleanup(conn);
        }
        http_protocol_strategy_destroy(conn->strategy);
        conn->strategy = NULL;
    }
    if (conn->parser != NULL) {
        parser_pool_return(conn->parser);
        conn->parser = NULL;
    }
    conn->current_request = NULL;

    conn->strategy      = http_protocol_strategy_websocket_create();
    conn->protocol_type = HTTP_PROTOCOL_WEBSOCKET;
    if (conn->strategy == NULL) {
        zend_throw_exception_ex(websocket_closed_exception_ce, 0,
            "WebSocket: out of memory installing strategy");
        w->closed = true;
        return false;
    }

    ws_session_t *session = ws_strategy_ensure_session(conn->strategy, conn);
    if (session == NULL) {
        zend_throw_exception_ex(websocket_closed_exception_ce, 0,
            "WebSocket: out of memory creating session");
        w->closed = true;
        return false;
    }
    w->session   = session;
    w->committed = true;

    /* Re-arm the connection's read loop. The earlier read callback
     * (under the H1 strategy) returned false from
     * handle_read_completion (parser_is_complete was true for the
     * upgrade GET) which stopped the multishot read; the new WS
     * strategy needs its own read subscription. */
    http_connection_read(conn);

    return true;
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

    websocket_upgrade_object *const u = Z_WEBSOCKET_UPGRADE_P(ZEND_THIS);
    if (u->committed) {
        zend_throw_exception_ex(websocket_exception_ce, 0,
            "WebSocketUpgrade: upgrade already committed; "
            "reject() must be called before any WebSocket I/O");
        RETURN_THROWS();
    }
    if (status < 400 || status > 599) {
        zend_argument_value_error(1,
            "must be a 4xx or 5xx HTTP status code");
        RETURN_THROWS();
    }
    if (u->reject_reason) {
        zend_string_release(u->reject_reason);
        u->reject_reason = NULL;
    }
    u->reject_status = (int)status;
    if (reason != NULL && ZSTR_LEN(reason) > 0) {
        u->reject_reason = zend_string_copy(reason);
    }
}

ZEND_METHOD(TrueAsync_WebSocketUpgrade, setSubprotocol)
{
    zend_string *name;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_STR(name)
    ZEND_PARSE_PARAMETERS_END();

    websocket_upgrade_object *const u = Z_WEBSOCKET_UPGRADE_P(ZEND_THIS);
    if (u->committed) {
        zend_throw_exception_ex(websocket_exception_ce, 0,
            "WebSocketUpgrade: upgrade already committed; "
            "setSubprotocol() must be called before any WebSocket I/O");
        RETURN_THROWS();
    }
    if (ZSTR_LEN(name) == 0) {
        zend_argument_value_error(1, "must not be empty");
        RETURN_THROWS();
    }
    if (u->subprotocol) {
        zend_string_release(u->subprotocol);
    }
    u->subprotocol = zend_string_copy(name);
}

/* Parse a comma-separated header value into individual tokens (trimmed),
 * appending each as a string element of `arr`. Used by
 * getOfferedSubprotocols / getOfferedExtensions; both header values
 * follow the comma-separated-list grammar of RFC 7230 §3.2. */
static void parse_header_token_list(HashTable *headers,
                                    const char *name, size_t name_len,
                                    zval *arr)
{
    if (headers == NULL) return;
    zval *val = zend_hash_str_find(headers, name, name_len);
    if (val == NULL || Z_TYPE_P(val) != IS_STRING) return;

    const char *s = Z_STRVAL_P(val);
    size_t len = Z_STRLEN_P(val), i = 0;
    while (i < len) {
        while (i < len && (s[i] == ' ' || s[i] == '\t' || s[i] == ',')) i++;
        size_t start = i;
        while (i < len && s[i] != ',') i++;
        size_t end = i;
        while (end > start && (s[end - 1] == ' ' || s[end - 1] == '\t')) end--;
        if (end > start) {
            zval token;
            ZVAL_STRINGL(&token, s + start, end - start);
            add_next_index_zval(arr, &token);
        }
    }
}

ZEND_METHOD(TrueAsync_WebSocketUpgrade, getOfferedSubprotocols)
{
    ZEND_PARSE_PARAMETERS_NONE();
    array_init(return_value);
    websocket_upgrade_object *const u = Z_WEBSOCKET_UPGRADE_P(ZEND_THIS);
    if (u->req != NULL) {
        parse_header_token_list(u->req->headers,
            "sec-websocket-protocol", sizeof("sec-websocket-protocol") - 1,
            return_value);
    }
}

ZEND_METHOD(TrueAsync_WebSocketUpgrade, getOfferedExtensions)
{
    ZEND_PARSE_PARAMETERS_NONE();
    array_init(return_value);
    websocket_upgrade_object *const u = Z_WEBSOCKET_UPGRADE_P(ZEND_THIS);
    if (u->req != NULL) {
        parse_header_token_list(u->req->headers,
            "sec-websocket-extensions", sizeof("sec-websocket-extensions") - 1,
            return_value);
    }
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

    /* Auto-commit on first WS I/O. If reject() was called, this
     * throws WebSocketClosedException and recv() propagates. */
    if (!w->committed && !ws_commit_upgrade(w)) {
        RETURN_THROWS();
    }
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

    if (!w->committed && !ws_commit_upgrade(w)) {
        return;   /* exception already set */
    }
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

    websocket_object *const w = Z_WEBSOCKET_P(ZEND_THIS);

    /* Idempotent — second close() is a no-op (RFC 6455 §5.5.1: a
     * peer should not respond with a duplicate close frame). */
    if (w->closed) {
        return;
    }
    /* close() before any other WS I/O still triggers the upgrade
     * commit — we need the session up to send a CLOSE frame. */
    if (!w->committed && !ws_commit_upgrade(w)) {
        return;   /* exception already set; the upgrade was rejected */
    }
    if (w->session == NULL) {
        return;
    }

    /* Resolve the close code from the union argument. The
     * WebSocketCloseCode enum carries the value as a long property;
     * raw int is accepted for application-specific 4000-4999 codes
     * (RFC 6455 §7.4.2). Default = 1000 Normal. */
    zend_long status = 1000;
    if (code != NULL && Z_TYPE_P(code) != IS_NULL) {
        if (Z_TYPE_P(code) == IS_OBJECT &&
            instanceof_function(Z_OBJCE_P(code), websocket_close_code_ce)) {
            zval *case_value = zend_enum_fetch_case_value(Z_OBJ_P(code));
            if (case_value != NULL && Z_TYPE_P(case_value) == IS_LONG) {
                status = Z_LVAL_P(case_value);
            }
        } else if (Z_TYPE_P(code) == IS_LONG) {
            status = Z_LVAL_P(code);
        } else {
            zend_argument_type_error(1,
                "must be of type TrueAsync\\WebSocketCloseCode|int");
            RETURN_THROWS();
        }
    }

    /* RFC 6455 §5.5.1: close-frame payload is a 2-byte status code
     * + optional UTF-8 reason. Total payload <= 125 bytes (control-
     * frame limit), so reason is capped at 123 bytes. */
    const char *reason_ptr = NULL;
    size_t      reason_len = 0;
    if (reason != NULL) {
        reason_ptr = ZSTR_VAL(reason);
        reason_len = ZSTR_LEN(reason);
        if (reason_len > 123) {
            reason_len = 123;
        }
    }

    ws_session_t *const s = w->session;
    int rc = wslay_event_queue_close(s->ctx, (uint16_t)status,
                                     (const uint8_t *)reason_ptr, reason_len);

    /* Drive wslay_event_send to push the close frame onto the wire,
     * subject to the same flusher discipline as ordinary send(). */
    if (rc == 0 && !s->flushing) {
        s->flushing = 1;
        wslay_event_send(s->ctx);
        s->flushing = 0;
    }

    /* Mark closed regardless of queue/send outcome — once close()
     * has been called, no further send/recv should succeed.
     * Subsequent recv() returns null after the FIFO drains
     * (peer_closed will fire from the read-side teardown). */
    w->closed = true;
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
