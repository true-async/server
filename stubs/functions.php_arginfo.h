/* This is a generated file, edit functions.php.stub.php instead.
 * Stub hash: a7839eaff617a7a2829aa8bd7fc545feb1b60d83 */

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_TYPE_MASK_EX(arginfo_TrueAsync_http_parse_request, 0, 1, TrueAsync\\HttpRequest, MAY_BE_FALSE)
	ZEND_ARG_TYPE_INFO(0, request, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_TrueAsync_server_dispose, 0, 0, IS_VOID, 0)
ZEND_END_ARG_INFO()

ZEND_FUNCTION(TrueAsync_http_parse_request);
ZEND_FUNCTION(TrueAsync_server_dispose);

static const zend_function_entry ext_functions[] = {
	ZEND_RAW_FENTRY(ZEND_NS_NAME("TrueAsync", "http_parse_request"), zif_TrueAsync_http_parse_request, arginfo_TrueAsync_http_parse_request, 0, NULL, NULL)
	ZEND_RAW_FENTRY(ZEND_NS_NAME("TrueAsync", "server_dispose"), zif_TrueAsync_server_dispose, arginfo_TrueAsync_server_dispose, 0, NULL, NULL)
	ZEND_FE_END
};
