/* This is a generated file, edit SendFileOptions.php instead. */
/* Manually maintained — gen_stub.php requires the tokenizer extension
 * which is unavailable in the bundled PHP build. Keep in sync with
 * stubs/SendFileOptions.php. */

ZEND_BEGIN_ARG_INFO_EX(arginfo_class_TrueAsync_SendFileOptions___construct, 0, 0, 0)
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, contentType,     IS_STRING, 1, "null")
	ZEND_ARG_OBJ_INFO_WITH_DEFAULT_VALUE(0, disposition,      TrueAsync\\SendFileDisposition, 0,
		"\\TrueAsync\\SendFileDisposition::INLINE")
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, downloadName,    IS_STRING, 1, "null")
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, cacheControl,    IS_STRING, 1, "null")
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, etag,            _IS_BOOL,  0, "true")
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, lastModified,    _IS_BOOL,  0, "true")
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, acceptRanges,    _IS_BOOL,  0, "true")
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, precompressed,   _IS_BOOL,  0, "true")
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, conditional,     _IS_BOOL,  0, "true")
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, deleteAfterSend, _IS_BOOL,  0, "false")
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, status,          IS_LONG,   1, "null")
ZEND_END_ARG_INFO()

ZEND_METHOD(TrueAsync_SendFileOptions, __construct);

static const zend_function_entry class_TrueAsync_SendFileOptions_methods[] = {
	ZEND_ME(TrueAsync_SendFileOptions, __construct,
		arginfo_class_TrueAsync_SendFileOptions___construct, ZEND_ACC_PUBLIC)
	ZEND_FE_END
};

static zend_class_entry *register_class_TrueAsync_SendFileDisposition(void)
{
	zend_class_entry *class_entry = zend_register_internal_enum(
		"TrueAsync\\SendFileDisposition", IS_STRING, NULL);

	zval case_INLINE_value;
	ZVAL_STR(&case_INLINE_value, zend_string_init("inline", 6, 1));
	zend_enum_add_case_cstr(class_entry, "INLINE", &case_INLINE_value);

	zval case_ATTACHMENT_value;
	ZVAL_STR(&case_ATTACHMENT_value, zend_string_init("attachment", 10, 1));
	zend_enum_add_case_cstr(class_entry, "ATTACHMENT", &case_ATTACHMENT_value);

	return class_entry;
}

static zend_class_entry *register_class_TrueAsync_SendFileOptions(zend_class_entry *disposition_ce)
{
	(void)disposition_ce;
	zend_class_entry ce, *class_entry;

	INIT_NS_CLASS_ENTRY(ce, "TrueAsync", "SendFileOptions",
		class_TrueAsync_SendFileOptions_methods);
	class_entry = zend_register_internal_class_with_flags(&ce, NULL,
		ZEND_ACC_FINAL | ZEND_ACC_NO_DYNAMIC_PROPERTIES |
		ZEND_ACC_NOT_SERIALIZABLE | ZEND_ACC_READONLY_CLASS);

	{
		zval default_zv;

		ZVAL_NULL(&default_zv);
		zend_string *p_ctype = zend_string_init("contentType", 11, 1);
		zend_declare_typed_property(class_entry, p_ctype, &default_zv,
			ZEND_ACC_PUBLIC | ZEND_ACC_READONLY, NULL,
			(zend_type) ZEND_TYPE_INIT_MASK(MAY_BE_STRING | MAY_BE_NULL));
		zend_string_release(p_ctype);

		ZVAL_NULL(&default_zv);
		zend_string *p_disp = zend_string_init("disposition", 11, 1);
		zend_string *p_disp_class =
			zend_string_init("TrueAsync\\SendFileDisposition", 29, 1);
		zend_declare_typed_property(class_entry, p_disp, &default_zv,
			ZEND_ACC_PUBLIC | ZEND_ACC_READONLY, NULL,
			(zend_type) ZEND_TYPE_INIT_CLASS(p_disp_class, 0, 0));
		zend_string_release(p_disp);

		ZVAL_NULL(&default_zv);
		zend_string *p_dn = zend_string_init("downloadName", 12, 1);
		zend_declare_typed_property(class_entry, p_dn, &default_zv,
			ZEND_ACC_PUBLIC | ZEND_ACC_READONLY, NULL,
			(zend_type) ZEND_TYPE_INIT_MASK(MAY_BE_STRING | MAY_BE_NULL));
		zend_string_release(p_dn);

		ZVAL_NULL(&default_zv);
		zend_string *p_cc = zend_string_init("cacheControl", 12, 1);
		zend_declare_typed_property(class_entry, p_cc, &default_zv,
			ZEND_ACC_PUBLIC | ZEND_ACC_READONLY, NULL,
			(zend_type) ZEND_TYPE_INIT_MASK(MAY_BE_STRING | MAY_BE_NULL));
		zend_string_release(p_cc);

		ZVAL_TRUE(&default_zv);
		zend_string *p_etag = zend_string_init("etag", 4, 1);
		zend_declare_typed_property(class_entry, p_etag, &default_zv,
			ZEND_ACC_PUBLIC | ZEND_ACC_READONLY, NULL,
			(zend_type) ZEND_TYPE_INIT_MASK(MAY_BE_BOOL));
		zend_string_release(p_etag);

		ZVAL_TRUE(&default_zv);
		zend_string *p_lm = zend_string_init("lastModified", 12, 1);
		zend_declare_typed_property(class_entry, p_lm, &default_zv,
			ZEND_ACC_PUBLIC | ZEND_ACC_READONLY, NULL,
			(zend_type) ZEND_TYPE_INIT_MASK(MAY_BE_BOOL));
		zend_string_release(p_lm);

		ZVAL_TRUE(&default_zv);
		zend_string *p_ar = zend_string_init("acceptRanges", 12, 1);
		zend_declare_typed_property(class_entry, p_ar, &default_zv,
			ZEND_ACC_PUBLIC | ZEND_ACC_READONLY, NULL,
			(zend_type) ZEND_TYPE_INIT_MASK(MAY_BE_BOOL));
		zend_string_release(p_ar);

		ZVAL_TRUE(&default_zv);
		zend_string *p_pc = zend_string_init("precompressed", 13, 1);
		zend_declare_typed_property(class_entry, p_pc, &default_zv,
			ZEND_ACC_PUBLIC | ZEND_ACC_READONLY, NULL,
			(zend_type) ZEND_TYPE_INIT_MASK(MAY_BE_BOOL));
		zend_string_release(p_pc);

		ZVAL_TRUE(&default_zv);
		zend_string *p_cond = zend_string_init("conditional", 11, 1);
		zend_declare_typed_property(class_entry, p_cond, &default_zv,
			ZEND_ACC_PUBLIC | ZEND_ACC_READONLY, NULL,
			(zend_type) ZEND_TYPE_INIT_MASK(MAY_BE_BOOL));
		zend_string_release(p_cond);

		ZVAL_FALSE(&default_zv);
		zend_string *p_del = zend_string_init("deleteAfterSend", 15, 1);
		zend_declare_typed_property(class_entry, p_del, &default_zv,
			ZEND_ACC_PUBLIC | ZEND_ACC_READONLY, NULL,
			(zend_type) ZEND_TYPE_INIT_MASK(MAY_BE_BOOL));
		zend_string_release(p_del);

		ZVAL_NULL(&default_zv);
		zend_string *p_st = zend_string_init("status", 6, 1);
		zend_declare_typed_property(class_entry, p_st, &default_zv,
			ZEND_ACC_PUBLIC | ZEND_ACC_READONLY, NULL,
			(zend_type) ZEND_TYPE_INIT_MASK(MAY_BE_LONG | MAY_BE_NULL));
		zend_string_release(p_st);
	}

	return class_entry;
}
