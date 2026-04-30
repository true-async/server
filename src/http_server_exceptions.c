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
#include "Zend/zend_async_API.h"
#include "php_http_server.h"

/* Include generated arginfo */
#include "../stubs/HttpServerExceptions.php_arginfo.h"

/* Class entries */
zend_class_entry *http_server_exception_ce;
zend_class_entry *http_server_runtime_exception_ce;
zend_class_entry *http_server_invalid_argument_exception_ce;
zend_class_entry *http_server_connection_exception_ce;
zend_class_entry *http_server_protocol_exception_ce;
zend_class_entry *http_server_timeout_exception_ce;
zend_class_entry *http_exception_ce;

/* {{{ http_server_exceptions_register */
void http_server_exceptions_register(void)
{
	/* Register base exception */
	http_server_exception_ce = register_class_TrueAsync_HttpServerException();

	/* Register derived exceptions */
	http_server_runtime_exception_ce = register_class_TrueAsync_HttpServerRuntimeException(http_server_exception_ce);
	http_server_invalid_argument_exception_ce = register_class_TrueAsync_HttpServerInvalidArgumentException(http_server_exception_ce);
	http_server_connection_exception_ce = register_class_TrueAsync_HttpServerConnectionException(http_server_exception_ce);
	http_server_protocol_exception_ce = register_class_TrueAsync_HttpServerProtocolException(http_server_exception_ce);
	http_server_timeout_exception_ce = register_class_TrueAsync_HttpServerTimeoutException(http_server_exception_ce);

	/* HttpException — separate hierarchy. Extends Async\AsyncCancellation
	 * (resolved at registration time inside the arginfo helper). User
	 * handlers throw it to set a precise HTTP error response; we also
	 * use it internally for cancelling in-flight handlers on parser
	 * errors. */
	http_exception_ce = register_class_TrueAsync_HttpException();
}
/* }}} */
