/* This is a generated file, edit HttpServerExceptions.php instead. */

/* HttpServerException - base class */
static zend_class_entry *register_class_TrueAsync_HttpServerException(void)
{
	zend_class_entry ce, *class_entry;

	INIT_NS_CLASS_ENTRY(ce, "TrueAsync", "HttpServerException", NULL);
	class_entry = zend_register_internal_class_ex(&ce, zend_ce_exception);

	return class_entry;
}

/* HttpServerRuntimeException */
static zend_class_entry *register_class_TrueAsync_HttpServerRuntimeException(zend_class_entry *parent_ce)
{
	zend_class_entry ce, *class_entry;

	INIT_NS_CLASS_ENTRY(ce, "TrueAsync", "HttpServerRuntimeException", NULL);
	class_entry = zend_register_internal_class_ex(&ce, parent_ce);
	class_entry->ce_flags |= ZEND_ACC_FINAL;

	return class_entry;
}

/* HttpServerInvalidArgumentException */
static zend_class_entry *register_class_TrueAsync_HttpServerInvalidArgumentException(zend_class_entry *parent_ce)
{
	zend_class_entry ce, *class_entry;

	INIT_NS_CLASS_ENTRY(ce, "TrueAsync", "HttpServerInvalidArgumentException", NULL);
	class_entry = zend_register_internal_class_ex(&ce, parent_ce);
	class_entry->ce_flags |= ZEND_ACC_FINAL;

	return class_entry;
}

/* HttpServerConnectionException */
static zend_class_entry *register_class_TrueAsync_HttpServerConnectionException(zend_class_entry *parent_ce)
{
	zend_class_entry ce, *class_entry;

	INIT_NS_CLASS_ENTRY(ce, "TrueAsync", "HttpServerConnectionException", NULL);
	class_entry = zend_register_internal_class_ex(&ce, parent_ce);
	class_entry->ce_flags |= ZEND_ACC_FINAL;

	return class_entry;
}

/* HttpServerProtocolException */
static zend_class_entry *register_class_TrueAsync_HttpServerProtocolException(zend_class_entry *parent_ce)
{
	zend_class_entry ce, *class_entry;

	INIT_NS_CLASS_ENTRY(ce, "TrueAsync", "HttpServerProtocolException", NULL);
	class_entry = zend_register_internal_class_ex(&ce, parent_ce);
	class_entry->ce_flags |= ZEND_ACC_FINAL;

	return class_entry;
}

/* HttpServerTimeoutException */
static zend_class_entry *register_class_TrueAsync_HttpServerTimeoutException(zend_class_entry *parent_ce)
{
	zend_class_entry ce, *class_entry;

	INIT_NS_CLASS_ENTRY(ce, "TrueAsync", "HttpServerTimeoutException", NULL);
	class_entry = zend_register_internal_class_ex(&ce, parent_ce);
	class_entry->ce_flags |= ZEND_ACC_FINAL;

	return class_entry;
}

/* HttpException — extends Async\AsyncCancellation so cancel-semantics
 * propagate through the existing TrueAsync cancellation chain. Parent
 * CE is looked up at registration time via the async API since it's
 * defined in the true_async ext (loaded before us per module deps). */
static zend_class_entry *register_class_TrueAsync_HttpException(void)
{
	zend_class_entry ce, *class_entry, *parent_ce;

	parent_ce = ZEND_ASYNC_GET_EXCEPTION_CE(ZEND_ASYNC_EXCEPTION_CANCELLATION);
	if (parent_ce == NULL) {
		/* Defensive — true_async should always be loaded if we are. */
		parent_ce = zend_ce_exception;
	}

	INIT_NS_CLASS_ENTRY(ce, "TrueAsync", "HttpException", NULL);
	class_entry = zend_register_internal_class_ex(&ce, parent_ce);

	return class_entry;
}
