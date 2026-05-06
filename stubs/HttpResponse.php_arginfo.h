/* This is a generated file, edit HttpResponse.php.stub.php instead.
 * Stub hash: 551944906eac9bec89e41dc304061db9ade9a70d */

ZEND_BEGIN_ARG_INFO_EX(arginfo_class_TrueAsync_HttpResponse___construct, 0, 0, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_TrueAsync_HttpResponse_setStatusCode, 0, 1, IS_STATIC, 0)
	ZEND_ARG_TYPE_INFO(0, code, IS_LONG, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_TrueAsync_HttpResponse_getStatusCode, 0, 0, IS_LONG, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_TrueAsync_HttpResponse_setReasonPhrase, 0, 1, IS_STATIC, 0)
	ZEND_ARG_TYPE_INFO(0, phrase, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_TrueAsync_HttpResponse_getReasonPhrase, 0, 0, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_TrueAsync_HttpResponse_setHeader, 0, 2, IS_STATIC, 0)
	ZEND_ARG_TYPE_INFO(0, name, IS_STRING, 0)
	ZEND_ARG_TYPE_MASK(0, value, MAY_BE_STRING|MAY_BE_ARRAY, NULL)
ZEND_END_ARG_INFO()

#define arginfo_class_TrueAsync_HttpResponse_addHeader arginfo_class_TrueAsync_HttpResponse_setHeader

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_TrueAsync_HttpResponse_hasHeader, 0, 1, _IS_BOOL, 0)
	ZEND_ARG_TYPE_INFO(0, name, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_TrueAsync_HttpResponse_getHeader, 0, 1, IS_STRING, 1)
	ZEND_ARG_TYPE_INFO(0, name, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_TrueAsync_HttpResponse_getHeaderLine, 0, 1, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO(0, name, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_TrueAsync_HttpResponse_getHeaders, 0, 0, IS_ARRAY, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_TrueAsync_HttpResponse_resetHeaders, 0, 0, IS_STATIC, 0)
ZEND_END_ARG_INFO()

/* Trailers (Step 5b — HTTP/2 only; dropped silently on HTTP/1). */
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_TrueAsync_HttpResponse_setTrailer, 0, 2, IS_STATIC, 0)
	ZEND_ARG_TYPE_INFO(0, name, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO(0, value, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_TrueAsync_HttpResponse_setTrailers, 0, 1, IS_STATIC, 0)
	ZEND_ARG_TYPE_INFO(0, trailers, IS_ARRAY, 0)
ZEND_END_ARG_INFO()

#define arginfo_class_TrueAsync_HttpResponse_resetTrailers arginfo_class_TrueAsync_HttpResponse_resetHeaders

#define arginfo_class_TrueAsync_HttpResponse_getTrailers arginfo_class_TrueAsync_HttpResponse_getHeaders

#define arginfo_class_TrueAsync_HttpResponse_getProtocolName arginfo_class_TrueAsync_HttpResponse_getReasonPhrase

#define arginfo_class_TrueAsync_HttpResponse_getProtocolVersion arginfo_class_TrueAsync_HttpResponse_getReasonPhrase

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_TrueAsync_HttpResponse_write, 0, 1, IS_STATIC, 0)
	ZEND_ARG_TYPE_INFO(0, data, IS_STRING, 0)
ZEND_END_ARG_INFO()

#define arginfo_class_TrueAsync_HttpResponse_getBody arginfo_class_TrueAsync_HttpResponse_getReasonPhrase

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_TrueAsync_HttpResponse_setBody, 0, 1, IS_STATIC, 0)
	ZEND_ARG_TYPE_INFO(0, body, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_TrueAsync_HttpResponse_getBodyStream, 0, 0, IS_MIXED, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_TrueAsync_HttpResponse_setBodyStream, 0, 1, IS_STATIC, 0)
	ZEND_ARG_TYPE_INFO(0, stream, IS_MIXED, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_TrueAsync_HttpResponse_json, 0, 1, IS_STATIC, 0)
	ZEND_ARG_TYPE_INFO(0, data, IS_MIXED, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_TrueAsync_HttpResponse_html, 0, 1, IS_STATIC, 0)
	ZEND_ARG_TYPE_INFO(0, html, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_TrueAsync_HttpResponse_redirect, 0, 1, IS_STATIC, 0)
	ZEND_ARG_TYPE_INFO(0, url, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, status, IS_LONG, 0, "302")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_TrueAsync_HttpResponse_end, 0, 0, IS_VOID, 0)
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, data, IS_STRING, 1, "null")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_TrueAsync_HttpResponse_isHeadersSent, 0, 0, _IS_BOOL, 0)
ZEND_END_ARG_INFO()

#define arginfo_class_TrueAsync_HttpResponse_isClosed arginfo_class_TrueAsync_HttpResponse_isHeadersSent

ZEND_METHOD(TrueAsync_HttpResponse, __construct);
ZEND_METHOD(TrueAsync_HttpResponse, setStatusCode);
ZEND_METHOD(TrueAsync_HttpResponse, getStatusCode);
ZEND_METHOD(TrueAsync_HttpResponse, setReasonPhrase);
ZEND_METHOD(TrueAsync_HttpResponse, getReasonPhrase);
ZEND_METHOD(TrueAsync_HttpResponse, setHeader);
ZEND_METHOD(TrueAsync_HttpResponse, addHeader);
ZEND_METHOD(TrueAsync_HttpResponse, hasHeader);
ZEND_METHOD(TrueAsync_HttpResponse, getHeader);
ZEND_METHOD(TrueAsync_HttpResponse, getHeaderLine);
ZEND_METHOD(TrueAsync_HttpResponse, getHeaders);
ZEND_METHOD(TrueAsync_HttpResponse, resetHeaders);
ZEND_METHOD(TrueAsync_HttpResponse, setTrailer);
ZEND_METHOD(TrueAsync_HttpResponse, setTrailers);
ZEND_METHOD(TrueAsync_HttpResponse, resetTrailers);
ZEND_METHOD(TrueAsync_HttpResponse, getTrailers);
ZEND_METHOD(TrueAsync_HttpResponse, getProtocolName);
ZEND_METHOD(TrueAsync_HttpResponse, getProtocolVersion);
ZEND_METHOD(TrueAsync_HttpResponse, write);
ZEND_METHOD(TrueAsync_HttpResponse, send);

/* send() has the same (string) → static signature as write(). */
#define arginfo_class_TrueAsync_HttpResponse_send arginfo_class_TrueAsync_HttpResponse_write
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_TrueAsync_HttpResponse_setNoCompression, 0, 0, IS_STATIC, 0)
ZEND_END_ARG_INFO()
ZEND_METHOD(TrueAsync_HttpResponse, setNoCompression);
ZEND_METHOD(TrueAsync_HttpResponse, getBody);
ZEND_METHOD(TrueAsync_HttpResponse, setBody);
ZEND_METHOD(TrueAsync_HttpResponse, getBodyStream);
ZEND_METHOD(TrueAsync_HttpResponse, setBodyStream);
ZEND_METHOD(TrueAsync_HttpResponse, json);
ZEND_METHOD(TrueAsync_HttpResponse, html);
ZEND_METHOD(TrueAsync_HttpResponse, redirect);
ZEND_METHOD(TrueAsync_HttpResponse, end);
ZEND_METHOD(TrueAsync_HttpResponse, isHeadersSent);
ZEND_METHOD(TrueAsync_HttpResponse, isClosed);

static const zend_function_entry class_TrueAsync_HttpResponse_methods[] = {
	ZEND_ME(TrueAsync_HttpResponse, __construct, arginfo_class_TrueAsync_HttpResponse___construct, ZEND_ACC_PRIVATE)
	ZEND_ME(TrueAsync_HttpResponse, setStatusCode, arginfo_class_TrueAsync_HttpResponse_setStatusCode, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpResponse, getStatusCode, arginfo_class_TrueAsync_HttpResponse_getStatusCode, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpResponse, setReasonPhrase, arginfo_class_TrueAsync_HttpResponse_setReasonPhrase, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpResponse, getReasonPhrase, arginfo_class_TrueAsync_HttpResponse_getReasonPhrase, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpResponse, setHeader, arginfo_class_TrueAsync_HttpResponse_setHeader, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpResponse, addHeader, arginfo_class_TrueAsync_HttpResponse_addHeader, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpResponse, hasHeader, arginfo_class_TrueAsync_HttpResponse_hasHeader, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpResponse, getHeader, arginfo_class_TrueAsync_HttpResponse_getHeader, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpResponse, getHeaderLine, arginfo_class_TrueAsync_HttpResponse_getHeaderLine, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpResponse, getHeaders, arginfo_class_TrueAsync_HttpResponse_getHeaders, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpResponse, resetHeaders, arginfo_class_TrueAsync_HttpResponse_resetHeaders, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpResponse, setTrailer, arginfo_class_TrueAsync_HttpResponse_setTrailer, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpResponse, setTrailers, arginfo_class_TrueAsync_HttpResponse_setTrailers, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpResponse, resetTrailers, arginfo_class_TrueAsync_HttpResponse_resetTrailers, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpResponse, getTrailers, arginfo_class_TrueAsync_HttpResponse_getTrailers, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpResponse, getProtocolName, arginfo_class_TrueAsync_HttpResponse_getProtocolName, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpResponse, getProtocolVersion, arginfo_class_TrueAsync_HttpResponse_getProtocolVersion, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpResponse, write, arginfo_class_TrueAsync_HttpResponse_write, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpResponse, send,  arginfo_class_TrueAsync_HttpResponse_send,  ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpResponse, setNoCompression, arginfo_class_TrueAsync_HttpResponse_setNoCompression, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpResponse, getBody, arginfo_class_TrueAsync_HttpResponse_getBody, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpResponse, setBody, arginfo_class_TrueAsync_HttpResponse_setBody, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpResponse, getBodyStream, arginfo_class_TrueAsync_HttpResponse_getBodyStream, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpResponse, setBodyStream, arginfo_class_TrueAsync_HttpResponse_setBodyStream, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpResponse, json, arginfo_class_TrueAsync_HttpResponse_json, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpResponse, html, arginfo_class_TrueAsync_HttpResponse_html, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpResponse, redirect, arginfo_class_TrueAsync_HttpResponse_redirect, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpResponse, end, arginfo_class_TrueAsync_HttpResponse_end, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpResponse, isHeadersSent, arginfo_class_TrueAsync_HttpResponse_isHeadersSent, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpResponse, isClosed, arginfo_class_TrueAsync_HttpResponse_isClosed, ZEND_ACC_PUBLIC)
	ZEND_FE_END
};

static zend_class_entry *register_class_TrueAsync_HttpResponse(void)
{
	zend_class_entry ce, *class_entry;

	INIT_NS_CLASS_ENTRY(ce, "TrueAsync", "HttpResponse", class_TrueAsync_HttpResponse_methods);
	class_entry = zend_register_internal_class_with_flags(&ce, NULL, ZEND_ACC_FINAL|ZEND_ACC_NO_DYNAMIC_PROPERTIES|ZEND_ACC_NOT_SERIALIZABLE);

	return class_entry;
}
