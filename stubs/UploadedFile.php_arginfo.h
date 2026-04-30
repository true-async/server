/* This is a generated file, edit UploadedFile.php.stub.php instead.
 * Stub hash: 7d45d069d9a9140e8bf78587d1c572a525769436 */

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_TrueAsync_UploadedFile_getStream, 0, 0, IS_MIXED, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_TrueAsync_UploadedFile_moveTo, 0, 1, IS_VOID, 0)
	ZEND_ARG_TYPE_INFO(0, targetPath, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, mode, IS_LONG, 0, "0644")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_TrueAsync_UploadedFile_getSize, 0, 0, IS_LONG, 1)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_TrueAsync_UploadedFile_getError, 0, 0, IS_LONG, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_TrueAsync_UploadedFile_getClientFilename, 0, 0, IS_STRING, 1)
ZEND_END_ARG_INFO()

#define arginfo_class_TrueAsync_UploadedFile_getClientMediaType arginfo_class_TrueAsync_UploadedFile_getClientFilename

#define arginfo_class_TrueAsync_UploadedFile_getClientCharset arginfo_class_TrueAsync_UploadedFile_getClientFilename

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_TrueAsync_UploadedFile_isReady, 0, 0, _IS_BOOL, 0)
ZEND_END_ARG_INFO()

#define arginfo_class_TrueAsync_UploadedFile_isValid arginfo_class_TrueAsync_UploadedFile_isReady

ZEND_METHOD(TrueAsync_UploadedFile, getStream);
ZEND_METHOD(TrueAsync_UploadedFile, moveTo);
ZEND_METHOD(TrueAsync_UploadedFile, getSize);
ZEND_METHOD(TrueAsync_UploadedFile, getError);
ZEND_METHOD(TrueAsync_UploadedFile, getClientFilename);
ZEND_METHOD(TrueAsync_UploadedFile, getClientMediaType);
ZEND_METHOD(TrueAsync_UploadedFile, getClientCharset);
ZEND_METHOD(TrueAsync_UploadedFile, isReady);
ZEND_METHOD(TrueAsync_UploadedFile, isValid);

static const zend_function_entry class_TrueAsync_UploadedFile_methods[] = {
	ZEND_ME(TrueAsync_UploadedFile, getStream, arginfo_class_TrueAsync_UploadedFile_getStream, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_UploadedFile, moveTo, arginfo_class_TrueAsync_UploadedFile_moveTo, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_UploadedFile, getSize, arginfo_class_TrueAsync_UploadedFile_getSize, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_UploadedFile, getError, arginfo_class_TrueAsync_UploadedFile_getError, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_UploadedFile, getClientFilename, arginfo_class_TrueAsync_UploadedFile_getClientFilename, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_UploadedFile, getClientMediaType, arginfo_class_TrueAsync_UploadedFile_getClientMediaType, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_UploadedFile, getClientCharset, arginfo_class_TrueAsync_UploadedFile_getClientCharset, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_UploadedFile, isReady, arginfo_class_TrueAsync_UploadedFile_isReady, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_UploadedFile, isValid, arginfo_class_TrueAsync_UploadedFile_isValid, ZEND_ACC_PUBLIC)
	ZEND_FE_END
};

static zend_class_entry *register_class_TrueAsync_UploadedFile(void)
{
	zend_class_entry ce, *class_entry;

	INIT_NS_CLASS_ENTRY(ce, "TrueAsync", "UploadedFile", class_TrueAsync_UploadedFile_methods);
	class_entry = zend_register_internal_class_with_flags(&ce, NULL, ZEND_ACC_FINAL);

	return class_entry;
}
