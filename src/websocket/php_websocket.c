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
#include "websocket/php_websocket.h"

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
    ws_throw_unimplemented("recv");
}

ZEND_METHOD(TrueAsync_WebSocket, send)
{
    zend_string *text;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_STR(text)
    ZEND_PARSE_PARAMETERS_END();
    (void)text;
    ws_throw_unimplemented("send");
}

ZEND_METHOD(TrueAsync_WebSocket, sendBinary)
{
    zend_string *data;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_STR(data)
    ZEND_PARSE_PARAMETERS_END();
    (void)data;
    ws_throw_unimplemented("sendBinary");
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
    /* Default-pre-implementation behaviour: report closed so naive
     * loops `while (!$ws->isClosed())` exit cleanly instead of
     * spinning when the rest of the API throws. */
    RETURN_TRUE;
}

ZEND_METHOD(TrueAsync_WebSocket, getSubprotocol)
{
    ZEND_PARSE_PARAMETERS_NONE();
    RETURN_NULL();
}

ZEND_METHOD(TrueAsync_WebSocket, getRemoteAddress)
{
    ZEND_PARSE_PARAMETERS_NONE();
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
