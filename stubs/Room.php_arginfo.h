/* This is a generated file, edit Room.php.stub.php instead.
 * Stub hash: b9f968d32ab6fb53948b2d65455ed558821af8e3 */

ZEND_BEGIN_ARG_INFO_EX(arginfo_class_TrueAsync_Room___construct, 0, 0, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_TrueAsync_Room_publish, 0, 1, IS_ARRAY, 0)
	ZEND_ARG_TYPE_INFO(0, message, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_TrueAsync_Room_publishBinary, 0, 1, IS_LONG, 0)
	ZEND_ARG_TYPE_INFO(0, data, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_TrueAsync_Room_trySend, 0, 1, _IS_BOOL, 0)
	ZEND_ARG_TYPE_INFO(0, message, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, timeoutMs, IS_LONG, 1, "null")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_TrueAsync_Room_send, 0, 1, IS_LONG, 0)
	ZEND_ARG_TYPE_INFO(0, message, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, timeoutMs, IS_LONG, 1, "null")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_TrueAsync_Room_subscribe, 0, 0, IS_VOID, 0)
ZEND_END_ARG_INFO()

#define arginfo_class_TrueAsync_Room_unsubscribe arginfo_class_TrueAsync_Room_subscribe

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_TrueAsync_Room_recv, 0, 0, IS_STRING, 1)
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, timeoutMs, IS_LONG, 1, "null")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_TrueAsync_Room_lostCount, 0, 0, IS_LONG, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_TrueAsync_Room_subscriberCount, 0, 0, IS_LONG, 0)
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, timeoutMs, IS_LONG, 0, "1000")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_TrueAsync_Room_name, 0, 0, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_METHOD(TrueAsync_Room, __construct);
ZEND_METHOD(TrueAsync_Room, publish);
ZEND_METHOD(TrueAsync_Room, publishBinary);
ZEND_METHOD(TrueAsync_Room, trySend);
ZEND_METHOD(TrueAsync_Room, send);
ZEND_METHOD(TrueAsync_Room, subscribe);
ZEND_METHOD(TrueAsync_Room, unsubscribe);
ZEND_METHOD(TrueAsync_Room, recv);
ZEND_METHOD(TrueAsync_Room, lostCount);
ZEND_METHOD(TrueAsync_Room, subscriberCount);
ZEND_METHOD(TrueAsync_Room, name);

static const zend_function_entry class_TrueAsync_Room_methods[] = {
	ZEND_ME(TrueAsync_Room, __construct, arginfo_class_TrueAsync_Room___construct, ZEND_ACC_PRIVATE)
	ZEND_ME(TrueAsync_Room, publish, arginfo_class_TrueAsync_Room_publish, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_Room, publishBinary, arginfo_class_TrueAsync_Room_publishBinary, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_Room, trySend, arginfo_class_TrueAsync_Room_trySend, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_Room, send, arginfo_class_TrueAsync_Room_send, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_Room, subscribe, arginfo_class_TrueAsync_Room_subscribe, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_Room, unsubscribe, arginfo_class_TrueAsync_Room_unsubscribe, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_Room, recv, arginfo_class_TrueAsync_Room_recv, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_Room, lostCount, arginfo_class_TrueAsync_Room_lostCount, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_Room, subscriberCount, arginfo_class_TrueAsync_Room_subscriberCount, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_Room, name, arginfo_class_TrueAsync_Room_name, ZEND_ACC_PUBLIC)
	ZEND_FE_END
};

static zend_class_entry *register_class_TrueAsync_Room(void)
{
	zend_class_entry ce, *class_entry;

	INIT_NS_CLASS_ENTRY(ce, "TrueAsync", "Room", class_TrueAsync_Room_methods);
	class_entry = zend_register_internal_class_with_flags(&ce, NULL, ZEND_ACC_FINAL|ZEND_ACC_NO_DYNAMIC_PROPERTIES|ZEND_ACC_NOT_SERIALIZABLE);

	return class_entry;
}
