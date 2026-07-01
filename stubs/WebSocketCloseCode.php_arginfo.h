/* This is a generated file, edit WebSocketCloseCode.php.stub.php instead.
 * Stub hash: 4a61deba6c591d8a8d80e33ee124a0c09d183d99 */

static zend_class_entry *register_class_TrueAsync_WebSocketCloseCode(void)
{
	zend_class_entry *class_entry = zend_register_internal_enum("TrueAsync\\WebSocketCloseCode", IS_LONG, NULL);

	zval enum_case_NORMAL_value;
	ZVAL_LONG(&enum_case_NORMAL_value, 1000);
	zend_enum_add_case_cstr(class_entry, "NORMAL", &enum_case_NORMAL_value);

	zval enum_case_GOING_AWAY_value;
	ZVAL_LONG(&enum_case_GOING_AWAY_value, 1001);
	zend_enum_add_case_cstr(class_entry, "GOING_AWAY", &enum_case_GOING_AWAY_value);

	zval enum_case_PROTOCOL_ERROR_value;
	ZVAL_LONG(&enum_case_PROTOCOL_ERROR_value, 1002);
	zend_enum_add_case_cstr(class_entry, "PROTOCOL_ERROR", &enum_case_PROTOCOL_ERROR_value);

	zval enum_case_UNSUPPORTED_DATA_value;
	ZVAL_LONG(&enum_case_UNSUPPORTED_DATA_value, 1003);
	zend_enum_add_case_cstr(class_entry, "UNSUPPORTED_DATA", &enum_case_UNSUPPORTED_DATA_value);

	zval enum_case_NO_STATUS_value;
	ZVAL_LONG(&enum_case_NO_STATUS_value, 1005);
	zend_enum_add_case_cstr(class_entry, "NO_STATUS", &enum_case_NO_STATUS_value);

	zval enum_case_ABNORMAL_CLOSURE_value;
	ZVAL_LONG(&enum_case_ABNORMAL_CLOSURE_value, 1006);
	zend_enum_add_case_cstr(class_entry, "ABNORMAL_CLOSURE", &enum_case_ABNORMAL_CLOSURE_value);

	zval enum_case_INVALID_FRAME_PAYLOAD_value;
	ZVAL_LONG(&enum_case_INVALID_FRAME_PAYLOAD_value, 1007);
	zend_enum_add_case_cstr(class_entry, "INVALID_FRAME_PAYLOAD", &enum_case_INVALID_FRAME_PAYLOAD_value);

	zval enum_case_POLICY_VIOLATION_value;
	ZVAL_LONG(&enum_case_POLICY_VIOLATION_value, 1008);
	zend_enum_add_case_cstr(class_entry, "POLICY_VIOLATION", &enum_case_POLICY_VIOLATION_value);

	zval enum_case_MESSAGE_TOO_BIG_value;
	ZVAL_LONG(&enum_case_MESSAGE_TOO_BIG_value, 1009);
	zend_enum_add_case_cstr(class_entry, "MESSAGE_TOO_BIG", &enum_case_MESSAGE_TOO_BIG_value);

	zval enum_case_MANDATORY_EXTENSION_value;
	ZVAL_LONG(&enum_case_MANDATORY_EXTENSION_value, 1010);
	zend_enum_add_case_cstr(class_entry, "MANDATORY_EXTENSION", &enum_case_MANDATORY_EXTENSION_value);

	zval enum_case_INTERNAL_SERVER_ERROR_value;
	ZVAL_LONG(&enum_case_INTERNAL_SERVER_ERROR_value, 1011);
	zend_enum_add_case_cstr(class_entry, "INTERNAL_SERVER_ERROR", &enum_case_INTERNAL_SERVER_ERROR_value);

	zval enum_case_TLS_HANDSHAKE_value;
	ZVAL_LONG(&enum_case_TLS_HANDSHAKE_value, 1015);
	zend_enum_add_case_cstr(class_entry, "TLS_HANDSHAKE", &enum_case_TLS_HANDSHAKE_value);

	return class_entry;
}
