/* This is a generated file, edit RateLimit.php.stub.php instead.
 * Stub hash: 604420188565a8b036dca3cd51ac94ccb2de330e */

ZEND_BEGIN_ARG_INFO_EX(arginfo_class_TrueAsync_RateLimit___construct, 0, 0, 2)
	ZEND_ARG_TYPE_INFO(0, requests, IS_LONG, 0)
	ZEND_ARG_TYPE_INFO(0, perSeconds, IS_LONG, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_TrueAsync_RateLimit_burst, 0, 1, IS_STATIC, 0)
	ZEND_ARG_TYPE_INFO(0, burst, IS_LONG, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_TrueAsync_RateLimit_paths, 0, 0, IS_STATIC, 0)
	ZEND_ARG_VARIADIC_TYPE_INFO(0, prefixes, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_TrueAsync_RateLimit_methods, 0, 0, IS_STATIC, 0)
	ZEND_ARG_VARIADIC_TYPE_INFO(0, methods, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_TrueAsync_RateLimit_maxKeys, 0, 1, IS_STATIC, 0)
	ZEND_ARG_TYPE_INFO(0, maxKeys, IS_LONG, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_TrueAsync_RateLimit_ipv6Prefix, 0, 1, IS_STATIC, 0)
	ZEND_ARG_TYPE_INFO(0, bits, IS_LONG, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_TrueAsync_RateLimit_name, 0, 1, IS_STATIC, 0)
	ZEND_ARG_TYPE_INFO(0, name, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_METHOD(TrueAsync_RateLimit, __construct);
ZEND_METHOD(TrueAsync_RateLimit, burst);
ZEND_METHOD(TrueAsync_RateLimit, paths);
ZEND_METHOD(TrueAsync_RateLimit, methods);
ZEND_METHOD(TrueAsync_RateLimit, maxKeys);
ZEND_METHOD(TrueAsync_RateLimit, ipv6Prefix);
ZEND_METHOD(TrueAsync_RateLimit, name);

static const zend_function_entry class_TrueAsync_RateLimit_methods[] = {
	ZEND_ME(TrueAsync_RateLimit, __construct, arginfo_class_TrueAsync_RateLimit___construct, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_RateLimit, burst, arginfo_class_TrueAsync_RateLimit_burst, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_RateLimit, paths, arginfo_class_TrueAsync_RateLimit_paths, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_RateLimit, methods, arginfo_class_TrueAsync_RateLimit_methods, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_RateLimit, maxKeys, arginfo_class_TrueAsync_RateLimit_maxKeys, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_RateLimit, ipv6Prefix, arginfo_class_TrueAsync_RateLimit_ipv6Prefix, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_RateLimit, name, arginfo_class_TrueAsync_RateLimit_name, ZEND_ACC_PUBLIC)
	ZEND_FE_END
};

static zend_class_entry *register_class_TrueAsync_RateLimit(void)
{
	zend_class_entry ce, *class_entry;

	INIT_NS_CLASS_ENTRY(ce, "TrueAsync", "RateLimit", class_TrueAsync_RateLimit_methods);
	class_entry = zend_register_internal_class_with_flags(&ce, NULL, ZEND_ACC_FINAL|ZEND_ACC_NO_DYNAMIC_PROPERTIES);

	return class_entry;
}
