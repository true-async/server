/* This is a generated file, edit WebSocketExceptions.php instead. */
/* Manually maintained — gen_stub.php requires the tokenizer extension
 * which is unavailable in the bundled PHP build. Keep in sync with
 * stubs/WebSocketExceptions.php. */

static zend_class_entry *register_class_TrueAsync_WebSocketException(zend_class_entry *parent_ce)
{
	zend_class_entry ce, *class_entry;

	INIT_NS_CLASS_ENTRY(ce, "TrueAsync", "WebSocketException", NULL);
	class_entry = zend_register_internal_class_ex(&ce, parent_ce);

	return class_entry;
}

static zend_class_entry *register_class_TrueAsync_WebSocketClosedException(zend_class_entry *parent_ce)
{
	zend_class_entry ce, *class_entry;

	INIT_NS_CLASS_ENTRY(ce, "TrueAsync", "WebSocketClosedException", NULL);
	class_entry = zend_register_internal_class_ex(&ce, parent_ce);
	class_entry->ce_flags |= ZEND_ACC_FINAL;

	zval default_close_code;
	ZVAL_LONG(&default_close_code, 0);
	zend_string *prop_close_code_name = zend_string_init("closeCode", sizeof("closeCode") - 1, 1);
	zend_declare_typed_property(class_entry, prop_close_code_name, &default_close_code,
		ZEND_ACC_PUBLIC | ZEND_ACC_READONLY, NULL,
		(zend_type) ZEND_TYPE_INIT_MASK(MAY_BE_LONG));
	zend_string_release(prop_close_code_name);

	zval default_close_reason;
	ZVAL_EMPTY_STRING(&default_close_reason);
	zend_string *prop_close_reason_name = zend_string_init("closeReason", sizeof("closeReason") - 1, 1);
	zend_declare_typed_property(class_entry, prop_close_reason_name, &default_close_reason,
		ZEND_ACC_PUBLIC | ZEND_ACC_READONLY, NULL,
		(zend_type) ZEND_TYPE_INIT_MASK(MAY_BE_STRING));
	zend_string_release(prop_close_reason_name);

	return class_entry;
}

static zend_class_entry *register_class_TrueAsync_WebSocketBackpressureException(zend_class_entry *parent_ce)
{
	zend_class_entry ce, *class_entry;

	INIT_NS_CLASS_ENTRY(ce, "TrueAsync", "WebSocketBackpressureException", NULL);
	class_entry = zend_register_internal_class_ex(&ce, parent_ce);
	class_entry->ce_flags |= ZEND_ACC_FINAL;

	return class_entry;
}

static zend_class_entry *register_class_TrueAsync_WebSocketConcurrentReadException(zend_class_entry *parent_ce)
{
	zend_class_entry ce, *class_entry;

	INIT_NS_CLASS_ENTRY(ce, "TrueAsync", "WebSocketConcurrentReadException", NULL);
	class_entry = zend_register_internal_class_ex(&ce, parent_ce);
	class_entry->ce_flags |= ZEND_ACC_FINAL;

	return class_entry;
}
