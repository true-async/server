/* This is a generated file, edit WebSocket.php instead. */
/* Manually maintained — gen_stub.php requires the tokenizer extension
 * which is unavailable in the bundled PHP build. Keep in sync with
 * stubs/WebSocket.php. */

ZEND_BEGIN_ARG_INFO_EX(arginfo_class_TrueAsync_WebSocket___construct, 0, 0, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_TYPE_MASK_EX(arginfo_class_TrueAsync_WebSocket_recv, 0, 0, TrueAsync\\WebSocketMessage, MAY_BE_NULL)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_TrueAsync_WebSocket_send, 0, 1, IS_VOID, 0)
	ZEND_ARG_TYPE_INFO(0, text, IS_STRING, 0)
ZEND_END_ARG_INFO()

#define arginfo_class_TrueAsync_WebSocket_sendBinary arginfo_class_TrueAsync_WebSocket_send

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_TrueAsync_WebSocket_ping, 0, 0, IS_VOID, 0)
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, payload, IS_STRING, 0, "\"\"")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_TrueAsync_WebSocket_close, 0, 0, IS_VOID, 0)
	ZEND_ARG_OBJ_TYPE_MASK(0, code, TrueAsync\\WebSocketCloseCode, MAY_BE_LONG, "TrueAsync\\WebSocketCloseCode::Normal")
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, reason, IS_STRING, 0, "\"\"")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_TrueAsync_WebSocket_isClosed, 0, 0, _IS_BOOL, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_TrueAsync_WebSocket_getSubprotocol, 0, 0, IS_STRING, 1)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_TrueAsync_WebSocket_getRemoteAddress, 0, 0, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_METHOD(TrueAsync_WebSocket, __construct);
ZEND_METHOD(TrueAsync_WebSocket, recv);
ZEND_METHOD(TrueAsync_WebSocket, send);
ZEND_METHOD(TrueAsync_WebSocket, sendBinary);
ZEND_METHOD(TrueAsync_WebSocket, ping);
ZEND_METHOD(TrueAsync_WebSocket, close);
ZEND_METHOD(TrueAsync_WebSocket, isClosed);
ZEND_METHOD(TrueAsync_WebSocket, getSubprotocol);
ZEND_METHOD(TrueAsync_WebSocket, getRemoteAddress);

static const zend_function_entry class_TrueAsync_WebSocket_methods[] = {
	ZEND_ME(TrueAsync_WebSocket, __construct, arginfo_class_TrueAsync_WebSocket___construct, ZEND_ACC_PRIVATE)
	ZEND_ME(TrueAsync_WebSocket, recv, arginfo_class_TrueAsync_WebSocket_recv, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_WebSocket, send, arginfo_class_TrueAsync_WebSocket_send, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_WebSocket, sendBinary, arginfo_class_TrueAsync_WebSocket_sendBinary, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_WebSocket, ping, arginfo_class_TrueAsync_WebSocket_ping, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_WebSocket, close, arginfo_class_TrueAsync_WebSocket_close, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_WebSocket, isClosed, arginfo_class_TrueAsync_WebSocket_isClosed, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_WebSocket, getSubprotocol, arginfo_class_TrueAsync_WebSocket_getSubprotocol, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_WebSocket, getRemoteAddress, arginfo_class_TrueAsync_WebSocket_getRemoteAddress, ZEND_ACC_PUBLIC)
	ZEND_FE_END
};

static zend_class_entry *register_class_TrueAsync_WebSocket(void)
{
	zend_class_entry ce, *class_entry;

	INIT_NS_CLASS_ENTRY(ce, "TrueAsync", "WebSocket", class_TrueAsync_WebSocket_methods);
	class_entry = zend_register_internal_class_with_flags(&ce, NULL,
		ZEND_ACC_FINAL | ZEND_ACC_NO_DYNAMIC_PROPERTIES | ZEND_ACC_NOT_SERIALIZABLE);

	return class_entry;
}
