/* This is a generated file, edit WebSocketCloseCode.php instead. */
/* Manually maintained — gen_stub.php requires the tokenizer extension
 * which is unavailable in the bundled PHP build. Keep in sync with
 * stubs/WebSocketCloseCode.php. */

static zend_class_entry *register_class_TrueAsync_WebSocketCloseCode(void)
{
	zend_class_entry *class_entry = zend_register_internal_enum(
		"TrueAsync\\WebSocketCloseCode", IS_LONG, NULL);

	zval v;
	ZVAL_LONG(&v, 1000); zend_enum_add_case_cstr(class_entry, "Normal",              &v);
	ZVAL_LONG(&v, 1001); zend_enum_add_case_cstr(class_entry, "GoingAway",           &v);
	ZVAL_LONG(&v, 1002); zend_enum_add_case_cstr(class_entry, "ProtocolError",       &v);
	ZVAL_LONG(&v, 1003); zend_enum_add_case_cstr(class_entry, "UnsupportedData",     &v);
	ZVAL_LONG(&v, 1005); zend_enum_add_case_cstr(class_entry, "NoStatus",            &v);
	ZVAL_LONG(&v, 1006); zend_enum_add_case_cstr(class_entry, "AbnormalClosure",     &v);
	ZVAL_LONG(&v, 1007); zend_enum_add_case_cstr(class_entry, "InvalidFramePayload", &v);
	ZVAL_LONG(&v, 1008); zend_enum_add_case_cstr(class_entry, "PolicyViolation",     &v);
	ZVAL_LONG(&v, 1009); zend_enum_add_case_cstr(class_entry, "MessageTooBig",       &v);
	ZVAL_LONG(&v, 1010); zend_enum_add_case_cstr(class_entry, "MandatoryExtension",  &v);
	ZVAL_LONG(&v, 1011); zend_enum_add_case_cstr(class_entry, "InternalServerError", &v);
	ZVAL_LONG(&v, 1015); zend_enum_add_case_cstr(class_entry, "TlsHandshake",        &v);

	return class_entry;
}
