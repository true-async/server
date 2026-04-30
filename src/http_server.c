/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include "php.h"
#include "php_ini.h"
#include "ext/standard/info.h"
#include "zend_smart_str.h"
#include "php_http_server.h"
#include "http1/http_parser.h"
#include "http_known_strings.h"
#include "log/http_log.h"

#ifdef HAVE_HTTP2
# include <nghttp2/nghttp2.h>
#endif
#ifdef HAVE_HTTP_SERVER_HTTP3
# include <ngtcp2/ngtcp2.h>
# include <nghttp3/nghttp3.h>
#endif
#ifdef HAVE_OPENSSL
# include <openssl/crypto.h>
# include <openssl/opensslv.h>
#endif

/* Include generated arginfo */
#include "../stubs/functions.php_arginfo.h"

/* Declare module globals */
ZEND_DECLARE_MODULE_GLOBALS(http_server)

extern zval* http_request_create_from_parsed(http_request_t *req);

/* PHP Functions */

/* {{{ proto void TrueAsync\server_dispose() */
ZEND_FUNCTION(TrueAsync_server_dispose)
{
	ZEND_PARSE_PARAMETERS_NONE();

	/* Clear parser pool */
	parser_pool_clear();

	/* Future: clear other internal state here */
}
/* }}} */

/* {{{ proto HttpRequest|false TrueAsync\http_parse_request(string $request) */
ZEND_FUNCTION(TrueAsync_http_parse_request)
{
	char *request_str;
	size_t request_len;

	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_STRING(request_str, request_len)
	ZEND_PARSE_PARAMETERS_END();

	/* Acquire parser from pool */
	http1_parser_t *parser_ctx = parser_pool_acquire();
	if (!parser_ctx) {
		RETURN_FALSE;
	}

	/* Parse request */
	int result = http_parser_execute(parser_ctx, request_str, request_len, NULL);

	/* On failure nobody took ownership of the partially-built request —
	 * reclaim it so parser_pool_return actually destroys it. Without this
	 * reclaim the request would leak (on_headers_complete, if it fired,
	 * flipped owns_request to false expecting a caller to pick it up). */
	if (result != 0 || !http_parser_is_complete(parser_ctx)) {
		parser_ctx->owns_request = (parser_ctx->request != NULL);
		parser_pool_return(parser_ctx);
		RETURN_FALSE;
	}

	/* Get parsed request */
	http_request_t *req = http_parser_get_request(parser_ctx);
	if (!req) {
		parser_pool_return(parser_ctx);
		RETURN_FALSE;
	}

	/* Validate HTTP version - we only support HTTP/1.0 and above */
	if (req->http_major < 1) {
		parser_ctx->owns_request = true;
		parser_pool_return(parser_ctx);
		RETURN_FALSE;
	}

	/* Hand the request struct to a PHP HttpRequest object. The parser
	 * still has a weak pointer to it (req->method != NULL marks it as
	 * "handed off"), so parser_pool_return will not free it when it
	 * resets the parser. */
	zval *obj = http_request_create_from_parsed(req);

	/* Return the parser to the pool. http_parser_reset_for_reuse sees
	 * method != NULL, skips http_request_destroy, and clears
	 * parser->request. The request struct now lives with the zval. */
	parser_pool_return(parser_ctx);

	/* Return object */
	RETVAL_OBJ(Z_OBJ_P(obj));
	efree(obj);
}
/* }}} */

/* {{{ php_http_server_init_globals */
static void php_http_server_init_globals(void *globals_ptr)
{
	zend_http_server_globals *globals = (zend_http_server_globals *)globals_ptr;
	memset(globals, 0, sizeof(zend_http_server_globals));
}
/* }}} */

/* Module entry point */
zend_module_entry http_server_module_entry = {
	STANDARD_MODULE_HEADER,
	"true_async_server",
	ext_functions,                             /* functions */
	PHP_MINIT(http_server),
	PHP_MSHUTDOWN(http_server),
	PHP_RINIT(http_server),
	PHP_RSHUTDOWN(http_server),
	PHP_MINFO(http_server),
	PHP_HTTP_SERVER_VERSION,
	PHP_MODULE_GLOBALS(http_server),           /* globals */
	php_http_server_init_globals,              /* globals init */
	NULL,                                      /* globals shutdown */
	NULL,                                      /* post deactivate */
	STANDARD_MODULE_PROPERTIES_EX
};

#ifdef COMPILE_DL_TRUE_ASYNC_SERVER
ZEND_GET_MODULE(http_server)
#endif

/* {{{ PHP_MINIT_FUNCTION */
PHP_MINIT_FUNCTION(http_server)
{
	/* Initialize module globals */
	ZEND_INIT_MODULE_GLOBALS(http_server, php_http_server_init_globals, NULL);

	/* Set parser pool configuration */
	HTTP_SERVER_G(parser_pool).capacity = 100;  /* TODO: Make configurable via INI */
	HTTP_SERVER_G(parser_pool).max_body_size = 10 * 1024 * 1024;  /* 10MB default */

	/* Intern common HTTP method tokens so the parser hot-path hands out
	 * zero-alloc zend_strings for GET/POST/... instead of emalloc'ing
	 * one per request. */
	http_known_strings_minit();
	http_log_minit();

	/* Phase 1: Register classes */
	http_request_class_register();
	uploaded_file_class_register();

	/* Phase 2: Register exceptions and server classes */
	http_server_exceptions_register();
	http_server_config_class_register();
	http_response_class_register();
	http_server_class_register();

	return SUCCESS;
}
/* }}} */

/* {{{ PHP_MSHUTDOWN_FUNCTION */
PHP_MSHUTDOWN_FUNCTION(http_server)
{
	http_log_mshutdown();
	return SUCCESS;
}
/* }}} */

/* {{{ PHP_RINIT_FUNCTION */
PHP_RINIT_FUNCTION(http_server)
{
#if defined(ZTS) && defined(COMPILE_DL_TRUE_ASYNC_SERVER)
	ZEND_TSRMLS_CACHE_UPDATE();
#endif

	/* Initialize per-request parser pool */
	size_t pool_capacity = HTTP_SERVER_G(parser_pool).capacity;
	HTTP_SERVER_G(parser_pool).parsers = emalloc(pool_capacity * sizeof(http1_parser_t*));
	HTTP_SERVER_G(parser_pool).count = 0;

	return SUCCESS;
}
/* }}} */

/* {{{ PHP_RSHUTDOWN_FUNCTION */
PHP_RSHUTDOWN_FUNCTION(http_server)
{
	/* Cleanup per-request parser pool */
	for (size_t i = 0; i < HTTP_SERVER_G(parser_pool).count; i++) {
		http_parser_destroy(HTTP_SERVER_G(parser_pool).parsers[i]);
	}

	if (HTTP_SERVER_G(parser_pool).parsers) {
		efree(HTTP_SERVER_G(parser_pool).parsers);
		HTTP_SERVER_G(parser_pool).parsers = NULL;
	}

	return SUCCESS;
}
/* }}} */

/* {{{ PHP_MINFO_FUNCTION */
PHP_MINFO_FUNCTION(http_server)
{
	php_info_print_table_start();
	php_info_print_table_header(2, "TrueAsync HTTP Server", "enabled");
	php_info_print_table_row(2, "Extension", "true_async_server");
	php_info_print_table_row(2, "Version", PHP_HTTP_SERVER_VERSION);

	smart_str protocols = {0};
	smart_str_appends(&protocols, "HTTP/1.1");
#ifdef HAVE_HTTP2
	smart_str_appends(&protocols, ", HTTP/2");
#endif
#ifdef HAVE_HTTP_SERVER_HTTP3
	smart_str_appends(&protocols, ", HTTP/3");
#endif
#ifdef HAVE_OPENSSL
	smart_str_appends(&protocols, ", TLS 1.2/1.3");
#endif
	smart_str_0(&protocols);
	php_info_print_table_row(2, "Protocols", ZSTR_VAL(protocols.s));
	smart_str_free(&protocols);

	php_info_print_table_end();

	php_info_print_table_start();
	php_info_print_table_header(2, "Library", "Version");
#ifdef HAVE_OPENSSL
	php_info_print_table_row(2, "OpenSSL", OpenSSL_version(OPENSSL_VERSION));
#endif
#ifdef HAVE_HTTP2
	php_info_print_table_row(2, "nghttp2", nghttp2_version(0)->version_str);
#endif
#ifdef HAVE_HTTP_SERVER_HTTP3
	php_info_print_table_row(2, "ngtcp2", ngtcp2_version(0)->version_str);
	php_info_print_table_row(2, "nghttp3", nghttp3_version(0)->version_str);
#endif
	php_info_print_table_end();
}
/* }}} */

/* Parser pool implementation */

extern void http_parser_reset_for_reuse(http1_parser_t *ctx);

/* Helper: Check if string buffer can be reused */
static inline bool can_reuse_string_buffer(zend_string *str)
{
    return str && !ZSTR_IS_INTERNED(str) && GC_REFCOUNT(str) == 1;
}

/* {{{ parser_pool_acquire - Get parser from pool or create new */
http1_parser_t* parser_pool_acquire(void)
{
	if (HTTP_SERVER_G(parser_pool).count > 0) {
		return HTTP_SERVER_G(parser_pool).parsers[--HTTP_SERVER_G(parser_pool).count];
	}

	return http_parser_create(HTTP_SERVER_G(parser_pool).max_body_size);
}
/* }}} */

/* {{{ parser_pool_return - Return parser to pool or destroy
 *
 * Defensive about shutdown ordering: in fast-shutdown mode the object
 * store is walked from `shutdown_executor`, and with Zend's fast path a
 * request object can be freed after the parser pool storage has already
 * been released (or before RINIT has run a fresh one). Fall back to
 * destroying the parser directly in that case — the pool is about to
 * go away anyway.
 */
void parser_pool_return(http1_parser_t *ctx)
{
	if (!ctx) {
		return;
	}

	if (HTTP_SERVER_G(parser_pool).parsers == NULL ||
		HTTP_SERVER_G(parser_pool).count >= HTTP_SERVER_G(parser_pool).capacity) {
		http_parser_destroy(ctx);
		return;
	}

	http_parser_reset_for_reuse(ctx);
	HTTP_SERVER_G(parser_pool).parsers[HTTP_SERVER_G(parser_pool).count++] = ctx;
}
/* }}} */

/* {{{ parser_pool_clear - Clear all parsers from pool */
void parser_pool_clear(void)
{
	for (size_t i = 0; i < HTTP_SERVER_G(parser_pool).count; i++) {
		http_parser_destroy(HTTP_SERVER_G(parser_pool).parsers[i]);
	}
	HTTP_SERVER_G(parser_pool).count = 0;
}
/* }}} */
