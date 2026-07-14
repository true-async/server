/* This is a generated file, edit WebSocket.php.stub.php instead.
 * Stub hash: a0dea67ed33d880640465ea00a15add56cf9526d */

ZEND_BEGIN_ARG_INFO_EX(arginfo_class_TrueAsync_WebSocket___construct, 0, 0, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_class_TrueAsync_WebSocket_recv, 0, 0, TrueAsync\\WebSocketMessage, 1)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_TrueAsync_WebSocket_send, 0, 1, IS_VOID, 0)
	ZEND_ARG_TYPE_INFO(0, text, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_TrueAsync_WebSocket_sendBinary, 0, 1, IS_VOID, 0)
	ZEND_ARG_TYPE_INFO(0, data, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_TrueAsync_WebSocket_trySend, 0, 1, _IS_BOOL, 0)
	ZEND_ARG_TYPE_INFO(0, text, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_TrueAsync_WebSocket_trySendBinary, 0, 1, _IS_BOOL, 0)
	ZEND_ARG_TYPE_INFO(0, data, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_TrueAsync_WebSocket_ping, 0, 0, IS_VOID, 0)
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, payload, IS_STRING, 0, "\'\'")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_TrueAsync_WebSocket_close, 0, 0, IS_VOID, 0)
	ZEND_ARG_OBJ_TYPE_MASK(0, code, TrueAsync\\WebSocketCloseCode, MAY_BE_LONG, "TrueAsync\\WebSocketCloseCode::NORMAL")
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, reason, IS_STRING, 0, "\'\'")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_TrueAsync_WebSocket_isClosed, 0, 0, _IS_BOOL, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_TrueAsync_WebSocket_getSubprotocol, 0, 0, IS_STRING, 1)
ZEND_END_ARG_INFO()

#define arginfo_class_TrueAsync_WebSocket_getRemoteAddress arginfo_class_TrueAsync_WebSocket_getSubprotocol

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_TrueAsync_WebSocket_getRemotePort, 0, 0, IS_LONG, 1)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_TrueAsync_WebSocket_subscribe, 0, 1, IS_VOID, 0)
	ZEND_ARG_TYPE_INFO(0, filter, IS_STRING, 0)
ZEND_END_ARG_INFO()

#define arginfo_class_TrueAsync_WebSocket_unsubscribe arginfo_class_TrueAsync_WebSocket_subscribe

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_TrueAsync_WebSocket_getTopics, 0, 0, IS_ARRAY, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_TrueAsync_WebSocket_publish, 0, 2, IS_LONG, 0)
	ZEND_ARG_TYPE_INFO(0, topic, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO(0, text, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, excludeSelf, _IS_BOOL, 0, "true")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_TrueAsync_WebSocket_publishBinary, 0, 2, IS_LONG, 0)
	ZEND_ARG_TYPE_INFO(0, topic, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO(0, data, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, excludeSelf, _IS_BOOL, 0, "true")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_TrueAsync_WebSocket_subscriberCount, 0, 1, IS_LONG, 0)
	ZEND_ARG_TYPE_INFO(0, topic, IS_STRING, 0)
ZEND_END_ARG_INFO()

#define arginfo_class_TrueAsync_WebSocket_current arginfo_class_TrueAsync_WebSocket_recv

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_TrueAsync_WebSocket_key, 0, 0, IS_LONG, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_TrueAsync_WebSocket_next, 0, 0, IS_VOID, 0)
ZEND_END_ARG_INFO()

#define arginfo_class_TrueAsync_WebSocket_rewind arginfo_class_TrueAsync_WebSocket_next

#define arginfo_class_TrueAsync_WebSocket_valid arginfo_class_TrueAsync_WebSocket_isClosed

ZEND_METHOD(TrueAsync_WebSocket, __construct);
ZEND_METHOD(TrueAsync_WebSocket, recv);
ZEND_METHOD(TrueAsync_WebSocket, send);
ZEND_METHOD(TrueAsync_WebSocket, sendBinary);
ZEND_METHOD(TrueAsync_WebSocket, trySend);
ZEND_METHOD(TrueAsync_WebSocket, trySendBinary);
ZEND_METHOD(TrueAsync_WebSocket, ping);
ZEND_METHOD(TrueAsync_WebSocket, close);
ZEND_METHOD(TrueAsync_WebSocket, isClosed);
ZEND_METHOD(TrueAsync_WebSocket, getSubprotocol);
ZEND_METHOD(TrueAsync_WebSocket, getRemoteAddress);
ZEND_METHOD(TrueAsync_WebSocket, getRemotePort);
ZEND_METHOD(TrueAsync_WebSocket, subscribe);
ZEND_METHOD(TrueAsync_WebSocket, unsubscribe);
ZEND_METHOD(TrueAsync_WebSocket, getTopics);
ZEND_METHOD(TrueAsync_WebSocket, publish);
ZEND_METHOD(TrueAsync_WebSocket, publishBinary);
ZEND_METHOD(TrueAsync_WebSocket, subscriberCount);
ZEND_METHOD(TrueAsync_WebSocket, current);
ZEND_METHOD(TrueAsync_WebSocket, key);
ZEND_METHOD(TrueAsync_WebSocket, next);
ZEND_METHOD(TrueAsync_WebSocket, rewind);
ZEND_METHOD(TrueAsync_WebSocket, valid);

static const zend_function_entry class_TrueAsync_WebSocket_methods[] = {
	ZEND_ME(TrueAsync_WebSocket, __construct, arginfo_class_TrueAsync_WebSocket___construct, ZEND_ACC_PRIVATE)
	ZEND_ME(TrueAsync_WebSocket, recv, arginfo_class_TrueAsync_WebSocket_recv, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_WebSocket, send, arginfo_class_TrueAsync_WebSocket_send, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_WebSocket, sendBinary, arginfo_class_TrueAsync_WebSocket_sendBinary, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_WebSocket, trySend, arginfo_class_TrueAsync_WebSocket_trySend, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_WebSocket, trySendBinary, arginfo_class_TrueAsync_WebSocket_trySendBinary, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_WebSocket, ping, arginfo_class_TrueAsync_WebSocket_ping, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_WebSocket, close, arginfo_class_TrueAsync_WebSocket_close, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_WebSocket, isClosed, arginfo_class_TrueAsync_WebSocket_isClosed, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_WebSocket, getSubprotocol, arginfo_class_TrueAsync_WebSocket_getSubprotocol, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_WebSocket, getRemoteAddress, arginfo_class_TrueAsync_WebSocket_getRemoteAddress, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_WebSocket, getRemotePort, arginfo_class_TrueAsync_WebSocket_getRemotePort, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_WebSocket, subscribe, arginfo_class_TrueAsync_WebSocket_subscribe, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_WebSocket, unsubscribe, arginfo_class_TrueAsync_WebSocket_unsubscribe, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_WebSocket, getTopics, arginfo_class_TrueAsync_WebSocket_getTopics, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_WebSocket, publish, arginfo_class_TrueAsync_WebSocket_publish, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_WebSocket, publishBinary, arginfo_class_TrueAsync_WebSocket_publishBinary, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_WebSocket, subscriberCount, arginfo_class_TrueAsync_WebSocket_subscriberCount, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_WebSocket, current, arginfo_class_TrueAsync_WebSocket_current, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_WebSocket, key, arginfo_class_TrueAsync_WebSocket_key, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_WebSocket, next, arginfo_class_TrueAsync_WebSocket_next, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_WebSocket, rewind, arginfo_class_TrueAsync_WebSocket_rewind, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_WebSocket, valid, arginfo_class_TrueAsync_WebSocket_valid, ZEND_ACC_PUBLIC)
	ZEND_FE_END
};

static zend_class_entry *register_class_TrueAsync_WebSocket(zend_class_entry *class_entry_Iterator)
{
	zend_class_entry ce, *class_entry;

	INIT_NS_CLASS_ENTRY(ce, "TrueAsync", "WebSocket", class_TrueAsync_WebSocket_methods);
	class_entry = zend_register_internal_class_with_flags(&ce, NULL, ZEND_ACC_FINAL);
	zend_class_implements(class_entry, 1, class_entry_Iterator);

	return class_entry;
}
