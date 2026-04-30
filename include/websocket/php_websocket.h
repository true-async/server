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

#endif /* PHP_WEBSOCKET_H */
