/* This is a generated file, edit StaticHandler.php instead. */
/* Manually maintained — gen_stub.php requires the tokenizer extension
 * which is unavailable in the bundled PHP build. Keep in sync with
 * stubs/StaticHandler.php. */

ZEND_BEGIN_ARG_INFO_EX(arginfo_class_TrueAsync_StaticHandler___construct, 0, 0, 2)
	ZEND_ARG_TYPE_INFO(0, urlPrefix, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO(0, rootDirectory, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_TrueAsync_StaticHandler_setIndexFiles, 0, 0, IS_STATIC, 0)
	ZEND_ARG_VARIADIC_TYPE_INFO(0, files, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_TrueAsync_StaticHandler_disableIndex, 0, 0, IS_STATIC, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_TrueAsync_StaticHandler_setOnMissing, 0, 1, IS_STATIC, 0)
	ZEND_ARG_OBJ_INFO(0, mode, TrueAsync\\StaticOnMissing, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_TrueAsync_StaticHandler_enablePrecompressed, 0, 0, IS_STATIC, 0)
	ZEND_ARG_VARIADIC_TYPE_INFO(0, encodings, IS_STRING, 0)
ZEND_END_ARG_INFO()

#define arginfo_class_TrueAsync_StaticHandler_disablePrecompressed arginfo_class_TrueAsync_StaticHandler_disableIndex

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_TrueAsync_StaticHandler_setDotfilePolicy, 0, 1, IS_STATIC, 0)
	ZEND_ARG_OBJ_INFO(0, policy, TrueAsync\\StaticDotfiles, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_TrueAsync_StaticHandler_setSymlinkPolicy, 0, 1, IS_STATIC, 0)
	ZEND_ARG_OBJ_INFO(0, policy, TrueAsync\\StaticSymlinks, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_TrueAsync_StaticHandler_hide, 0, 0, IS_STATIC, 0)
	ZEND_ARG_VARIADIC_TYPE_INFO(0, globs, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_TrueAsync_StaticHandler_setEtagEnabled, 0, 1, IS_STATIC, 0)
	ZEND_ARG_TYPE_INFO(0, enabled, _IS_BOOL, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_TrueAsync_StaticHandler_setCacheControl, 0, 1, IS_STATIC, 0)
	ZEND_ARG_TYPE_INFO(0, value, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_TrueAsync_StaticHandler_setOpenFileCache, 0, 1, IS_STATIC, 0)
	ZEND_ARG_TYPE_INFO(0, maxEntries, IS_LONG, 0)
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, ttlSeconds, IS_LONG, 0, "60")
ZEND_END_ARG_INFO()

#define arginfo_class_TrueAsync_StaticHandler_disableOpenFileCache arginfo_class_TrueAsync_StaticHandler_disableIndex

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_TrueAsync_StaticHandler_setHeader, 0, 2, IS_STATIC, 0)
	ZEND_ARG_TYPE_INFO(0, name, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO(0, value, IS_STRING, 0)
ZEND_END_ARG_INFO()

#define arginfo_class_TrueAsync_StaticHandler_setBrowseEnabled arginfo_class_TrueAsync_StaticHandler_setEtagEnabled

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_TrueAsync_StaticHandler_setMimeType, 0, 2, IS_STATIC, 0)
	ZEND_ARG_TYPE_INFO(0, extension, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO(0, contentType, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_TrueAsync_StaticHandler_getUrlPrefix, 0, 0, IS_STRING, 0)
ZEND_END_ARG_INFO()

#define arginfo_class_TrueAsync_StaticHandler_getRootDirectory arginfo_class_TrueAsync_StaticHandler_getUrlPrefix

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_TrueAsync_StaticHandler_isLocked, 0, 0, _IS_BOOL, 0)
ZEND_END_ARG_INFO()


ZEND_METHOD(TrueAsync_StaticHandler, __construct);
ZEND_METHOD(TrueAsync_StaticHandler, setIndexFiles);
ZEND_METHOD(TrueAsync_StaticHandler, disableIndex);
ZEND_METHOD(TrueAsync_StaticHandler, setOnMissing);
ZEND_METHOD(TrueAsync_StaticHandler, enablePrecompressed);
ZEND_METHOD(TrueAsync_StaticHandler, disablePrecompressed);
ZEND_METHOD(TrueAsync_StaticHandler, setDotfilePolicy);
ZEND_METHOD(TrueAsync_StaticHandler, setSymlinkPolicy);
ZEND_METHOD(TrueAsync_StaticHandler, hide);
ZEND_METHOD(TrueAsync_StaticHandler, setEtagEnabled);
ZEND_METHOD(TrueAsync_StaticHandler, setCacheControl);
ZEND_METHOD(TrueAsync_StaticHandler, setOpenFileCache);
ZEND_METHOD(TrueAsync_StaticHandler, disableOpenFileCache);
ZEND_METHOD(TrueAsync_StaticHandler, setHeader);
ZEND_METHOD(TrueAsync_StaticHandler, setBrowseEnabled);
ZEND_METHOD(TrueAsync_StaticHandler, setMimeType);
ZEND_METHOD(TrueAsync_StaticHandler, getUrlPrefix);
ZEND_METHOD(TrueAsync_StaticHandler, getRootDirectory);
ZEND_METHOD(TrueAsync_StaticHandler, isLocked);


static const zend_function_entry class_TrueAsync_StaticHandler_methods[] = {
	ZEND_ME(TrueAsync_StaticHandler, __construct,         arginfo_class_TrueAsync_StaticHandler___construct,         ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_StaticHandler, setIndexFiles,       arginfo_class_TrueAsync_StaticHandler_setIndexFiles,       ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_StaticHandler, disableIndex,        arginfo_class_TrueAsync_StaticHandler_disableIndex,        ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_StaticHandler, setOnMissing,        arginfo_class_TrueAsync_StaticHandler_setOnMissing,        ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_StaticHandler, enablePrecompressed, arginfo_class_TrueAsync_StaticHandler_enablePrecompressed, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_StaticHandler, disablePrecompressed,arginfo_class_TrueAsync_StaticHandler_disablePrecompressed,ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_StaticHandler, setDotfilePolicy,    arginfo_class_TrueAsync_StaticHandler_setDotfilePolicy,    ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_StaticHandler, setSymlinkPolicy,    arginfo_class_TrueAsync_StaticHandler_setSymlinkPolicy,    ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_StaticHandler, hide,                arginfo_class_TrueAsync_StaticHandler_hide,                ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_StaticHandler, setEtagEnabled,      arginfo_class_TrueAsync_StaticHandler_setEtagEnabled,      ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_StaticHandler, setCacheControl,     arginfo_class_TrueAsync_StaticHandler_setCacheControl,     ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_StaticHandler, setOpenFileCache,    arginfo_class_TrueAsync_StaticHandler_setOpenFileCache,    ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_StaticHandler, disableOpenFileCache,arginfo_class_TrueAsync_StaticHandler_disableOpenFileCache,ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_StaticHandler, setHeader,           arginfo_class_TrueAsync_StaticHandler_setHeader,           ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_StaticHandler, setBrowseEnabled,    arginfo_class_TrueAsync_StaticHandler_setBrowseEnabled,    ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_StaticHandler, setMimeType,         arginfo_class_TrueAsync_StaticHandler_setMimeType,         ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_StaticHandler, getUrlPrefix,        arginfo_class_TrueAsync_StaticHandler_getUrlPrefix,        ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_StaticHandler, getRootDirectory,    arginfo_class_TrueAsync_StaticHandler_getRootDirectory,    ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_StaticHandler, isLocked,            arginfo_class_TrueAsync_StaticHandler_isLocked,            ZEND_ACC_PUBLIC)
	ZEND_FE_END
};


/* Class registration */
static zend_class_entry *register_class_TrueAsync_StaticHandler(void)
{
	zend_class_entry ce, *class_entry;

	INIT_NS_CLASS_ENTRY(ce, "TrueAsync", "StaticHandler", class_TrueAsync_StaticHandler_methods);
	class_entry = zend_register_internal_class_with_flags(&ce, NULL, ZEND_ACC_FINAL|ZEND_ACC_NO_DYNAMIC_PROPERTIES);

	return class_entry;
}


/* Enums — manually emitted, mirror stubs/LogSeverity.php_arginfo.h pattern. */

static zend_class_entry *register_class_TrueAsync_StaticOnMissing(void)
{
	zend_class_entry *class_entry = zend_register_internal_enum(
		"TrueAsync\\StaticOnMissing", IS_LONG, NULL);

	zval case_NOT_FOUND_value; ZVAL_LONG(&case_NOT_FOUND_value, 0);
	zval case_NEXT_value;      ZVAL_LONG(&case_NEXT_value,      1);
	zend_enum_add_case_cstr(class_entry, "NOT_FOUND", &case_NOT_FOUND_value);
	zend_enum_add_case_cstr(class_entry, "NEXT",      &case_NEXT_value);

	return class_entry;
}

static zend_class_entry *register_class_TrueAsync_StaticDotfiles(void)
{
	zend_class_entry *class_entry = zend_register_internal_enum(
		"TrueAsync\\StaticDotfiles", IS_LONG, NULL);

	zval case_DENY_value;   ZVAL_LONG(&case_DENY_value,   0);
	zval case_ALLOW_value;  ZVAL_LONG(&case_ALLOW_value,  1);
	zval case_IGNORE_value; ZVAL_LONG(&case_IGNORE_value, 2);
	zend_enum_add_case_cstr(class_entry, "DENY",   &case_DENY_value);
	zend_enum_add_case_cstr(class_entry, "ALLOW",  &case_ALLOW_value);
	zend_enum_add_case_cstr(class_entry, "IGNORE", &case_IGNORE_value);

	return class_entry;
}

static zend_class_entry *register_class_TrueAsync_StaticSymlinks(void)
{
	zend_class_entry *class_entry = zend_register_internal_enum(
		"TrueAsync\\StaticSymlinks", IS_LONG, NULL);

	zval case_REJECT_value;      ZVAL_LONG(&case_REJECT_value,      0);
	zval case_FOLLOW_value;      ZVAL_LONG(&case_FOLLOW_value,      1);
	zval case_OWNER_MATCH_value; ZVAL_LONG(&case_OWNER_MATCH_value, 2);
	zend_enum_add_case_cstr(class_entry, "REJECT",      &case_REJECT_value);
	zend_enum_add_case_cstr(class_entry, "FOLLOW",      &case_FOLLOW_value);
	zend_enum_add_case_cstr(class_entry, "OWNER_MATCH", &case_OWNER_MATCH_value);

	return class_entry;
}
