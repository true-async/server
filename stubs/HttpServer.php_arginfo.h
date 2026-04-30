/* This is a generated file, edit HttpServer.php instead. */

/* Constructor */
ZEND_BEGIN_ARG_INFO_EX(arginfo_class_TrueAsync_HttpServer___construct, 0, 0, 1)
	ZEND_ARG_OBJ_INFO(0, config, TrueAsync\\HttpServerConfig, 0)
ZEND_END_ARG_INFO()

/* addHttpHandler */
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_TrueAsync_HttpServer_addHttpHandler, 0, 1, IS_STATIC, 0)
	ZEND_ARG_TYPE_INFO(0, handler, IS_CALLABLE, 0)
ZEND_END_ARG_INFO()

/* addWebSocketHandler */
#define arginfo_class_TrueAsync_HttpServer_addWebSocketHandler arginfo_class_TrueAsync_HttpServer_addHttpHandler

/* addHttp2Handler */
#define arginfo_class_TrueAsync_HttpServer_addHttp2Handler arginfo_class_TrueAsync_HttpServer_addHttpHandler

/* addGrpcHandler */
#define arginfo_class_TrueAsync_HttpServer_addGrpcHandler arginfo_class_TrueAsync_HttpServer_addHttpHandler

/* start */
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_TrueAsync_HttpServer_start, 0, 0, _IS_BOOL, 0)
ZEND_END_ARG_INFO()

/* stop */
#define arginfo_class_TrueAsync_HttpServer_stop arginfo_class_TrueAsync_HttpServer_start

/* isRunning */
#define arginfo_class_TrueAsync_HttpServer_isRunning arginfo_class_TrueAsync_HttpServer_start

/* getTelemetry */
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_TrueAsync_HttpServer_getTelemetry, 0, 0, IS_ARRAY, 0)
ZEND_END_ARG_INFO()

/* resetTelemetry */
#define arginfo_class_TrueAsync_HttpServer_resetTelemetry arginfo_class_TrueAsync_HttpServer_start

/* getConfig */
ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_class_TrueAsync_HttpServer_getConfig, 0, 0, TrueAsync\\HttpServerConfig, 0)
ZEND_END_ARG_INFO()

/* getHttp3Stats */
#define arginfo_class_TrueAsync_HttpServer_getHttp3Stats arginfo_class_TrueAsync_HttpServer_getTelemetry

/* Method declarations */
ZEND_METHOD(TrueAsync_HttpServer, __construct);
ZEND_METHOD(TrueAsync_HttpServer, addHttpHandler);
ZEND_METHOD(TrueAsync_HttpServer, addWebSocketHandler);
ZEND_METHOD(TrueAsync_HttpServer, addHttp2Handler);
ZEND_METHOD(TrueAsync_HttpServer, addGrpcHandler);
ZEND_METHOD(TrueAsync_HttpServer, start);
ZEND_METHOD(TrueAsync_HttpServer, stop);
ZEND_METHOD(TrueAsync_HttpServer, isRunning);
ZEND_METHOD(TrueAsync_HttpServer, getTelemetry);
ZEND_METHOD(TrueAsync_HttpServer, resetTelemetry);
ZEND_METHOD(TrueAsync_HttpServer, getConfig);
ZEND_METHOD(TrueAsync_HttpServer, getHttp3Stats);

/* Method table */
static const zend_function_entry class_TrueAsync_HttpServer_methods[] = {
	ZEND_ME(TrueAsync_HttpServer, __construct, arginfo_class_TrueAsync_HttpServer___construct, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpServer, addHttpHandler, arginfo_class_TrueAsync_HttpServer_addHttpHandler, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpServer, addWebSocketHandler, arginfo_class_TrueAsync_HttpServer_addWebSocketHandler, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpServer, addHttp2Handler, arginfo_class_TrueAsync_HttpServer_addHttp2Handler, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpServer, addGrpcHandler, arginfo_class_TrueAsync_HttpServer_addGrpcHandler, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpServer, start, arginfo_class_TrueAsync_HttpServer_start, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpServer, stop, arginfo_class_TrueAsync_HttpServer_stop, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpServer, isRunning, arginfo_class_TrueAsync_HttpServer_isRunning, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpServer, getTelemetry, arginfo_class_TrueAsync_HttpServer_getTelemetry, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpServer, resetTelemetry, arginfo_class_TrueAsync_HttpServer_resetTelemetry, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpServer, getConfig, arginfo_class_TrueAsync_HttpServer_getConfig, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpServer, getHttp3Stats, arginfo_class_TrueAsync_HttpServer_getHttp3Stats, ZEND_ACC_PUBLIC)
	ZEND_FE_END
};

/* Class registration */
static zend_class_entry *register_class_TrueAsync_HttpServer(void)
{
	zend_class_entry ce, *class_entry;

	INIT_NS_CLASS_ENTRY(ce, "TrueAsync", "HttpServer", class_TrueAsync_HttpServer_methods);
	class_entry = zend_register_internal_class_with_flags(&ce, NULL, ZEND_ACC_FINAL|ZEND_ACC_NO_DYNAMIC_PROPERTIES);

	return class_entry;
}
