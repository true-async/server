/* This is a generated file, edit HttpRequest.php.stub.php instead.
 * Stub hash: 2c87774dca350a1a6cba6a7be3e34465aef246c7 */

ZEND_BEGIN_ARG_INFO_EX(arginfo_class_TrueAsync_HttpRequest___construct, 0, 0, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_TrueAsync_HttpRequest_getMethod, 0, 0, IS_STRING, 0)
ZEND_END_ARG_INFO()

#define arginfo_class_TrueAsync_HttpRequest_getUri arginfo_class_TrueAsync_HttpRequest_getMethod

#define arginfo_class_TrueAsync_HttpRequest_getHttpVersion arginfo_class_TrueAsync_HttpRequest_getMethod

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_TrueAsync_HttpRequest_hasHeader, 0, 1, _IS_BOOL, 0)
	ZEND_ARG_TYPE_INFO(0, name, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_TrueAsync_HttpRequest_getHeader, 0, 1, IS_STRING, 1)
	ZEND_ARG_TYPE_INFO(0, name, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_TrueAsync_HttpRequest_getHeaderLine, 0, 1, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO(0, name, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_TrueAsync_HttpRequest_getHeaders, 0, 0, IS_ARRAY, 0)
ZEND_END_ARG_INFO()

#define arginfo_class_TrueAsync_HttpRequest_getBody arginfo_class_TrueAsync_HttpRequest_getMethod

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_TrueAsync_HttpRequest_hasBody, 0, 0, _IS_BOOL, 0)
ZEND_END_ARG_INFO()

#define arginfo_class_TrueAsync_HttpRequest_isKeepAlive arginfo_class_TrueAsync_HttpRequest_hasBody

#define arginfo_class_TrueAsync_HttpRequest_getPost arginfo_class_TrueAsync_HttpRequest_getHeaders

#define arginfo_class_TrueAsync_HttpRequest_getFiles arginfo_class_TrueAsync_HttpRequest_getHeaders

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_class_TrueAsync_HttpRequest_getFile, 0, 1, TrueAsync\\\125ploadedFile, 1)
	ZEND_ARG_TYPE_INFO(0, name, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_TrueAsync_HttpRequest_getContentType, 0, 0, IS_STRING, 1)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_TrueAsync_HttpRequest_getContentLength, 0, 0, IS_LONG, 1)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_TrueAsync_HttpRequest_awaitBody, 0, 0, IS_STATIC, 0)
ZEND_END_ARG_INFO()

#define arginfo_class_TrueAsync_HttpRequest_getTraceParent  arginfo_class_TrueAsync_HttpRequest_getContentType
#define arginfo_class_TrueAsync_HttpRequest_getTraceState   arginfo_class_TrueAsync_HttpRequest_getContentType
#define arginfo_class_TrueAsync_HttpRequest_getTraceId      arginfo_class_TrueAsync_HttpRequest_getContentType
#define arginfo_class_TrueAsync_HttpRequest_getSpanId       arginfo_class_TrueAsync_HttpRequest_getContentType
#define arginfo_class_TrueAsync_HttpRequest_getTraceFlags   arginfo_class_TrueAsync_HttpRequest_getContentLength

ZEND_METHOD(TrueAsync_HttpRequest, __construct);
ZEND_METHOD(TrueAsync_HttpRequest, getMethod);
ZEND_METHOD(TrueAsync_HttpRequest, getUri);
ZEND_METHOD(TrueAsync_HttpRequest, getHttpVersion);
ZEND_METHOD(TrueAsync_HttpRequest, hasHeader);
ZEND_METHOD(TrueAsync_HttpRequest, getHeader);
ZEND_METHOD(TrueAsync_HttpRequest, getHeaderLine);
ZEND_METHOD(TrueAsync_HttpRequest, getHeaders);
ZEND_METHOD(TrueAsync_HttpRequest, getBody);
ZEND_METHOD(TrueAsync_HttpRequest, hasBody);
ZEND_METHOD(TrueAsync_HttpRequest, isKeepAlive);
ZEND_METHOD(TrueAsync_HttpRequest, getPost);
ZEND_METHOD(TrueAsync_HttpRequest, getFiles);
ZEND_METHOD(TrueAsync_HttpRequest, getFile);
ZEND_METHOD(TrueAsync_HttpRequest, getContentType);
ZEND_METHOD(TrueAsync_HttpRequest, getContentLength);
ZEND_METHOD(TrueAsync_HttpRequest, getTraceParent);
ZEND_METHOD(TrueAsync_HttpRequest, getTraceState);
ZEND_METHOD(TrueAsync_HttpRequest, getTraceId);
ZEND_METHOD(TrueAsync_HttpRequest, getSpanId);
ZEND_METHOD(TrueAsync_HttpRequest, getTraceFlags);
ZEND_METHOD(TrueAsync_HttpRequest, awaitBody);

static const zend_function_entry class_TrueAsync_HttpRequest_methods[] = {
	ZEND_ME(TrueAsync_HttpRequest, __construct, arginfo_class_TrueAsync_HttpRequest___construct, ZEND_ACC_PRIVATE)
	ZEND_ME(TrueAsync_HttpRequest, getMethod, arginfo_class_TrueAsync_HttpRequest_getMethod, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpRequest, getUri, arginfo_class_TrueAsync_HttpRequest_getUri, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpRequest, getHttpVersion, arginfo_class_TrueAsync_HttpRequest_getHttpVersion, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpRequest, hasHeader, arginfo_class_TrueAsync_HttpRequest_hasHeader, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpRequest, getHeader, arginfo_class_TrueAsync_HttpRequest_getHeader, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpRequest, getHeaderLine, arginfo_class_TrueAsync_HttpRequest_getHeaderLine, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpRequest, getHeaders, arginfo_class_TrueAsync_HttpRequest_getHeaders, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpRequest, getBody, arginfo_class_TrueAsync_HttpRequest_getBody, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpRequest, hasBody, arginfo_class_TrueAsync_HttpRequest_hasBody, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpRequest, isKeepAlive, arginfo_class_TrueAsync_HttpRequest_isKeepAlive, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpRequest, getPost, arginfo_class_TrueAsync_HttpRequest_getPost, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpRequest, getFiles, arginfo_class_TrueAsync_HttpRequest_getFiles, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpRequest, getFile, arginfo_class_TrueAsync_HttpRequest_getFile, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpRequest, getContentType, arginfo_class_TrueAsync_HttpRequest_getContentType, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpRequest, getContentLength, arginfo_class_TrueAsync_HttpRequest_getContentLength, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpRequest, getTraceParent, arginfo_class_TrueAsync_HttpRequest_getTraceParent, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpRequest, getTraceState,  arginfo_class_TrueAsync_HttpRequest_getTraceState,  ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpRequest, getTraceId,     arginfo_class_TrueAsync_HttpRequest_getTraceId,     ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpRequest, getSpanId,      arginfo_class_TrueAsync_HttpRequest_getSpanId,      ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpRequest, getTraceFlags,  arginfo_class_TrueAsync_HttpRequest_getTraceFlags,  ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpRequest, awaitBody, arginfo_class_TrueAsync_HttpRequest_awaitBody, ZEND_ACC_PUBLIC)
	ZEND_FE_END
};

static zend_class_entry *register_class_TrueAsync_HttpRequest(void)
{
	zend_class_entry ce, *class_entry;

	INIT_NS_CLASS_ENTRY(ce, "TrueAsync", "HttpRequest", class_TrueAsync_HttpRequest_methods);
	class_entry = zend_register_internal_class_with_flags(&ce, NULL, ZEND_ACC_FINAL);

	return class_entry;
}
