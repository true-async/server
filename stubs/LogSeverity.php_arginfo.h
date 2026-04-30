/* This is a generated file, edit LogSeverity.php instead. */
/* Manually maintained — gen_stub.php requires the tokenizer extension
 * which is unavailable in the bundled PHP build. Keep in sync with
 * stubs/LogSeverity.php. */

static zend_class_entry *register_class_TrueAsync_LogSeverity(void)
{
	zend_class_entry *class_entry = zend_register_internal_enum(
		"TrueAsync\\LogSeverity", IS_LONG, NULL);

	zval case_OFF_value;   ZVAL_LONG(&case_OFF_value,   0);
	zval case_DEBUG_value; ZVAL_LONG(&case_DEBUG_value, 5);
	zval case_INFO_value;  ZVAL_LONG(&case_INFO_value,  9);
	zval case_WARN_value;  ZVAL_LONG(&case_WARN_value,  13);
	zval case_ERROR_value; ZVAL_LONG(&case_ERROR_value, 17);
	zend_enum_add_case_cstr(class_entry, "OFF",   &case_OFF_value);
	zend_enum_add_case_cstr(class_entry, "DEBUG", &case_DEBUG_value);
	zend_enum_add_case_cstr(class_entry, "INFO",  &case_INFO_value);
	zend_enum_add_case_cstr(class_entry, "WARN",  &case_WARN_value);
	zend_enum_add_case_cstr(class_entry, "ERROR", &case_ERROR_value);

	return class_entry;
}
