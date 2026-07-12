/* This is a generated file, edit WebSocketRoom.php.stub.php instead.
 * Stub hash: a1f6479913c7265d68491e433d81251156311aca */

ZEND_BEGIN_ARG_INFO_EX(arginfo_class_TrueAsync_WebSocketRoom___construct, 0, 0, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_TrueAsync_WebSocketRoom_join, 0, 1, IS_VOID, 0)
	ZEND_ARG_OBJ_INFO(0, ws, TrueAsync\\WebSocket, 0)
ZEND_END_ARG_INFO()

#define arginfo_class_TrueAsync_WebSocketRoom_leave arginfo_class_TrueAsync_WebSocketRoom_join

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_TrueAsync_WebSocketRoom_broadcast, 0, 1, IS_LONG, 0)
	ZEND_ARG_TYPE_INFO(0, text, IS_STRING, 0)
	ZEND_ARG_OBJ_INFO_WITH_DEFAULT_VALUE(0, except, TrueAsync\\WebSocket, 1, "null")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_TrueAsync_WebSocketRoom_broadcastBinary, 0, 1, IS_LONG, 0)
	ZEND_ARG_TYPE_INFO(0, data, IS_STRING, 0)
	ZEND_ARG_OBJ_INFO_WITH_DEFAULT_VALUE(0, except, TrueAsync\\WebSocket, 1, "null")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_TrueAsync_WebSocketRoom_count, 0, 0, IS_LONG, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_TrueAsync_WebSocketRoom_getName, 0, 0, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_METHOD(TrueAsync_WebSocketRoom, __construct);
ZEND_METHOD(TrueAsync_WebSocketRoom, join);
ZEND_METHOD(TrueAsync_WebSocketRoom, leave);
ZEND_METHOD(TrueAsync_WebSocketRoom, broadcast);
ZEND_METHOD(TrueAsync_WebSocketRoom, broadcastBinary);
ZEND_METHOD(TrueAsync_WebSocketRoom, count);
ZEND_METHOD(TrueAsync_WebSocketRoom, getName);

static const zend_function_entry class_TrueAsync_WebSocketRoom_methods[] = {
	ZEND_ME(TrueAsync_WebSocketRoom, __construct, arginfo_class_TrueAsync_WebSocketRoom___construct, ZEND_ACC_PRIVATE)
	ZEND_ME(TrueAsync_WebSocketRoom, join, arginfo_class_TrueAsync_WebSocketRoom_join, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_WebSocketRoom, leave, arginfo_class_TrueAsync_WebSocketRoom_leave, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_WebSocketRoom, broadcast, arginfo_class_TrueAsync_WebSocketRoom_broadcast, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_WebSocketRoom, broadcastBinary, arginfo_class_TrueAsync_WebSocketRoom_broadcastBinary, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_WebSocketRoom, count, arginfo_class_TrueAsync_WebSocketRoom_count, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_WebSocketRoom, getName, arginfo_class_TrueAsync_WebSocketRoom_getName, ZEND_ACC_PUBLIC)
	ZEND_FE_END
};

static zend_class_entry *register_class_TrueAsync_WebSocketRoom(void)
{
	zend_class_entry ce, *class_entry;

	INIT_NS_CLASS_ENTRY(ce, "TrueAsync", "WebSocketRoom", class_TrueAsync_WebSocketRoom_methods);
	class_entry = zend_register_internal_class_with_flags(&ce, NULL, ZEND_ACC_FINAL);

	return class_entry;
}
