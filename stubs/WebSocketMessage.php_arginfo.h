/* This is a generated file, edit WebSocketMessage.php instead. */
/* Manually maintained — gen_stub.php requires the tokenizer extension
 * which is unavailable in the bundled PHP build. Keep in sync with
 * stubs/WebSocketMessage.php. */

ZEND_BEGIN_ARG_INFO_EX(arginfo_class_TrueAsync_WebSocketMessage___construct, 0, 0, 0)
ZEND_END_ARG_INFO()

ZEND_METHOD(TrueAsync_WebSocketMessage, __construct);

static const zend_function_entry class_TrueAsync_WebSocketMessage_methods[] = {
	ZEND_ME(TrueAsync_WebSocketMessage, __construct, arginfo_class_TrueAsync_WebSocketMessage___construct, ZEND_ACC_PRIVATE)
	ZEND_FE_END
};

static zend_class_entry *register_class_TrueAsync_WebSocketMessage(void)
{
	zend_class_entry ce, *class_entry;

	INIT_NS_CLASS_ENTRY(ce, "TrueAsync", "WebSocketMessage", class_TrueAsync_WebSocketMessage_methods);
	class_entry = zend_register_internal_class_with_flags(&ce, NULL,
		ZEND_ACC_FINAL | ZEND_ACC_NO_DYNAMIC_PROPERTIES | ZEND_ACC_NOT_SERIALIZABLE);

	zval default_data;
	ZVAL_EMPTY_STRING(&default_data);
	zend_string *prop_data_name = zend_string_init("data", sizeof("data") - 1, 1);
	zend_declare_typed_property(class_entry, prop_data_name, &default_data,
		ZEND_ACC_PUBLIC | ZEND_ACC_READONLY, NULL,
		(zend_type) ZEND_TYPE_INIT_MASK(MAY_BE_STRING));
	zend_string_release(prop_data_name);

	zval default_binary;
	ZVAL_FALSE(&default_binary);
	zend_string *prop_binary_name = zend_string_init("binary", sizeof("binary") - 1, 1);
	zend_declare_typed_property(class_entry, prop_binary_name, &default_binary,
		ZEND_ACC_PUBLIC | ZEND_ACC_READONLY, NULL,
		(zend_type) ZEND_TYPE_INIT_MASK(MAY_BE_BOOL));
	zend_string_release(prop_binary_name);

	return class_entry;
}
