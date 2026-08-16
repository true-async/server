/* This is a generated file, edit WebSocketExceptions.php.stub.php instead.
 * Stub hash: f51617574ca6467dc5d2790500f3f601005292f7 */

static zend_class_entry *register_class_TrueAsync_WebSocketException(zend_class_entry *class_entry_TrueAsync_HttpServerException)
{
	zend_class_entry ce, *class_entry;

	INIT_NS_CLASS_ENTRY(ce, "TrueAsync", "WebSocketException", NULL);
	class_entry = zend_register_internal_class_with_flags(&ce, class_entry_TrueAsync_HttpServerException, ZEND_ACC_NO_DYNAMIC_PROPERTIES);

	return class_entry;
}

static zend_class_entry *register_class_TrueAsync_WebSocketClosedException(zend_class_entry *class_entry_TrueAsync_WebSocketException)
{
	zend_class_entry ce, *class_entry;

	INIT_NS_CLASS_ENTRY(ce, "TrueAsync", "WebSocketClosedException", NULL);
	class_entry = zend_register_internal_class_with_flags(&ce, class_entry_TrueAsync_WebSocketException, ZEND_ACC_FINAL);

	zval property_closeCode_default_value;
	ZVAL_UNDEF(&property_closeCode_default_value);
	zend_string *property_closeCode_name = zend_string_init("closeCode", sizeof("closeCode") - 1, true);
	zend_declare_typed_property(class_entry, property_closeCode_name, &property_closeCode_default_value, ZEND_ACC_PUBLIC|ZEND_ACC_READONLY, NULL, (zend_type) ZEND_TYPE_INIT_MASK(MAY_BE_LONG));
	zend_string_release_ex(property_closeCode_name, true);

	zval property_closeReason_default_value;
	ZVAL_UNDEF(&property_closeReason_default_value);
	zend_string *property_closeReason_name = zend_string_init("closeReason", sizeof("closeReason") - 1, true);
	zend_declare_typed_property(class_entry, property_closeReason_name, &property_closeReason_default_value, ZEND_ACC_PUBLIC|ZEND_ACC_READONLY, NULL, (zend_type) ZEND_TYPE_INIT_MASK(MAY_BE_STRING));
	zend_string_release_ex(property_closeReason_name, true);

	return class_entry;
}

static zend_class_entry *register_class_TrueAsync_WebSocketBackpressureException(zend_class_entry *class_entry_TrueAsync_WebSocketException)
{
	zend_class_entry ce, *class_entry;

	INIT_NS_CLASS_ENTRY(ce, "TrueAsync", "WebSocketBackpressureException", NULL);
	class_entry = zend_register_internal_class_with_flags(&ce, class_entry_TrueAsync_WebSocketException, ZEND_ACC_FINAL);

	return class_entry;
}

static zend_class_entry *register_class_TrueAsync_WebSocketConcurrentReadException(zend_class_entry *class_entry_TrueAsync_WebSocketException)
{
	zend_class_entry ce, *class_entry;

	INIT_NS_CLASS_ENTRY(ce, "TrueAsync", "WebSocketConcurrentReadException", NULL);
	class_entry = zend_register_internal_class_with_flags(&ce, class_entry_TrueAsync_WebSocketException, ZEND_ACC_FINAL);

	return class_entry;
}
