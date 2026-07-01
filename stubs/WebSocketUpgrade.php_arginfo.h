/* This is a generated file, edit WebSocketUpgrade.php instead. */
/* Manually maintained — gen_stub.php requires the tokenizer extension
 * which is unavailable in the bundled PHP build. Keep in sync with
 * stubs/WebSocketUpgrade.php. */

ZEND_BEGIN_ARG_INFO_EX(arginfo_class_TrueAsync_WebSocketUpgrade___construct, 0, 0, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_TrueAsync_WebSocketUpgrade_reject, 0, 1, IS_VOID, 0)
	ZEND_ARG_TYPE_INFO(0, status, IS_LONG, 0)
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, reason, IS_STRING, 0, "\"\"")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_TrueAsync_WebSocketUpgrade_setSubprotocol, 0, 1, IS_VOID, 0)
	ZEND_ARG_TYPE_INFO(0, name, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_TrueAsync_WebSocketUpgrade_getOfferedSubprotocols, 0, 0, IS_ARRAY, 0)
ZEND_END_ARG_INFO()

#define arginfo_class_TrueAsync_WebSocketUpgrade_getOfferedExtensions arginfo_class_TrueAsync_WebSocketUpgrade_getOfferedSubprotocols

ZEND_METHOD(TrueAsync_WebSocketUpgrade, __construct);
ZEND_METHOD(TrueAsync_WebSocketUpgrade, reject);
ZEND_METHOD(TrueAsync_WebSocketUpgrade, setSubprotocol);
ZEND_METHOD(TrueAsync_WebSocketUpgrade, getOfferedSubprotocols);
ZEND_METHOD(TrueAsync_WebSocketUpgrade, getOfferedExtensions);

static const zend_function_entry class_TrueAsync_WebSocketUpgrade_methods[] = {
	ZEND_ME(TrueAsync_WebSocketUpgrade, __construct, arginfo_class_TrueAsync_WebSocketUpgrade___construct, ZEND_ACC_PRIVATE)
	ZEND_ME(TrueAsync_WebSocketUpgrade, reject, arginfo_class_TrueAsync_WebSocketUpgrade_reject, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_WebSocketUpgrade, setSubprotocol, arginfo_class_TrueAsync_WebSocketUpgrade_setSubprotocol, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_WebSocketUpgrade, getOfferedSubprotocols, arginfo_class_TrueAsync_WebSocketUpgrade_getOfferedSubprotocols, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_WebSocketUpgrade, getOfferedExtensions, arginfo_class_TrueAsync_WebSocketUpgrade_getOfferedExtensions, ZEND_ACC_PUBLIC)
	ZEND_FE_END
};

static zend_class_entry *register_class_TrueAsync_WebSocketUpgrade(void)
{
	zend_class_entry ce, *class_entry;

	INIT_NS_CLASS_ENTRY(ce, "TrueAsync", "WebSocketUpgrade", class_TrueAsync_WebSocketUpgrade_methods);
	class_entry = zend_register_internal_class_with_flags(&ce, NULL,
		ZEND_ACC_FINAL | ZEND_ACC_NO_DYNAMIC_PROPERTIES | ZEND_ACC_NOT_SERIALIZABLE);

	return class_entry;
}
