/* This is a generated file, edit RoomExceptions.php.stub.php instead.
 * Stub hash: fb6755ef95eb5e362fa563cb9ff0705fc7db4402 */

static zend_class_entry *register_class_TrueAsync_RoomDeliveryException(zend_class_entry *class_entry_TrueAsync_HttpServerException)
{
	zend_class_entry ce, *class_entry;

	INIT_NS_CLASS_ENTRY(ce, "TrueAsync", "RoomDeliveryException", NULL);
	class_entry = zend_register_internal_class_with_flags(&ce, class_entry_TrueAsync_HttpServerException, ZEND_ACC_FINAL|ZEND_ACC_NO_DYNAMIC_PROPERTIES);

	zval property_delivered_default_value;
	ZVAL_UNDEF(&property_delivered_default_value);
	zend_string *property_delivered_name = zend_string_init("delivered", sizeof("delivered") - 1, true);
	zend_declare_typed_property(class_entry, property_delivered_name, &property_delivered_default_value, ZEND_ACC_PUBLIC|ZEND_ACC_READONLY, NULL, (zend_type) ZEND_TYPE_INIT_MASK(MAY_BE_LONG));
	zend_string_release_ex(property_delivered_name, true);

	zval property_pending_default_value;
	ZVAL_UNDEF(&property_pending_default_value);
	zend_string *property_pending_name = zend_string_init("pending", sizeof("pending") - 1, true);
	zend_declare_typed_property(class_entry, property_pending_name, &property_pending_default_value, ZEND_ACC_PUBLIC|ZEND_ACC_READONLY, NULL, (zend_type) ZEND_TYPE_INIT_MASK(MAY_BE_LONG));
	zend_string_release_ex(property_pending_name, true);

	return class_entry;
}
