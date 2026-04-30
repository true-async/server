/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

/*
 * Minimal PHP runtime initialization for unit tests
 * Based on FrankenPHP SAPI initialization
 */

#include "php.h"
#include "zend.h"
#include "zend_alloc.h"
#include "zend_signal.h"
#include "SAPI.h"
#include "TSRM.h"
#include "php_main.h"
#include "php_variables.h"

/* Weak stubs for extension-level class entries referenced by production
 * source files that are linked into unit tests. The real definitions
 * live in src/http_server_exceptions.c and are populated at MINIT in
 * the loaded extension; unit tests run against libphp only, so we
 * supply NULL defaults here. Individual tests may override by defining
 * the same symbol non-weakly. */
zend_class_entry *http_exception_ce __attribute__((weak)) = NULL;

/* Weak stubs for extension-level helpers that would normally come from
 * src/http_response.c / src/http_server_class.c. Unit tests link only
 * the protocol-strategy + session TUs, so these cross-TU references
 * would otherwise fail to resolve. Any individual test that exercises
 * these paths can supply a strong override. */
struct http_response_stream_ops;
typedef struct http_response_stream_ops http_response_stream_ops_t;

__attribute__((weak)) void http_response_install_stream_ops(zend_object *obj,
                                                            const http_response_stream_ops_t *ops,
                                                            void *ctx) {
    (void)obj; (void)ops; (void)ctx;
}

__attribute__((weak)) bool http_response_is_streaming(zend_object *obj) {
    (void)obj;
    return false;
}

/* http_server_object is forward-declared only; use void* signatures in
 * the weak stubs since we don't actually touch the struct. The real
 * declarations take http_server_object* / http_connection_t*, which
 * are pointer-compatible at the ABI level. */
__attribute__((weak)) bool http_server_should_drain_now(void *server,
                                                        void *conn) {
    (void)server; (void)conn;
    return false;
}

__attribute__((weak)) void http_server_on_h2_goaway_sent(void *server) {
    (void)server;
}

__attribute__((weak)) void http_server_on_h1_connection_close_sent(void *server) {
    (void)server;
}

/* Minimal SAPI callbacks */
static size_t php_test_sapi_ub_write(const char *str, size_t str_length) {
    (void)str;
    (void)str_length;
    return str_length;
}

static void php_test_sapi_flush(void *server_context) {
    (void)server_context;
}

static size_t php_test_sapi_read(char *buffer, size_t count) {
    (void)buffer;
    (void)count;
    return 0;
}

static char* php_test_sapi_read_cookies(void) {
    return NULL;
}

static void php_test_sapi_register_variables(zval *track_vars_array) {
    (void)track_vars_array;
}

static int php_test_sapi_send_headers(sapi_headers_struct *sapi_headers) {
    (void)sapi_headers;
    return SAPI_HEADER_SENT_SUCCESSFULLY;
}

static void php_test_sapi_send_header(sapi_header_struct *sapi_header, void *server_context) {
    (void)sapi_header;
    (void)server_context;
}

static void php_test_sapi_log_message(const char *message, int syslog_type_int) {
    (void)message;
    (void)syslog_type_int;
}

/* SAPI startup - calls php_module_startup */
static int php_test_sapi_startup(sapi_module_struct *sapi_module) {
    return php_module_startup(sapi_module, NULL);
}

/* SAPI shutdown wrapper */
static int php_test_sapi_shutdown_wrapper(sapi_module_struct *sapi_module) {
    (void)sapi_module;
    php_module_shutdown();
    return SUCCESS;
}

/* SAPI deactivate */
static int php_test_sapi_deactivate(void) {
    return SUCCESS;
}

/* SAPI module definition */
static sapi_module_struct php_test_sapi_module = {
    "test",                         /* name */
    "PHP Unit Test SAPI",           /* pretty name */

    php_test_sapi_startup,          /* startup */
    php_test_sapi_shutdown_wrapper, /* shutdown */

    NULL,                           /* activate */
    php_test_sapi_deactivate,       /* deactivate */

    php_test_sapi_ub_write,         /* unbuffered write */
    php_test_sapi_flush,            /* flush */
    NULL,                           /* get uid */
    NULL,                           /* getenv */

    php_error,                      /* error handler */

    NULL,                           /* header handler */
    php_test_sapi_send_headers,     /* send headers handler */
    php_test_sapi_send_header,      /* send header handler */

    php_test_sapi_read,             /* read POST data */
    php_test_sapi_read_cookies,     /* read Cookies */

    php_test_sapi_register_variables, /* register server variables */
    php_test_sapi_log_message,      /* Log message */
    NULL,                           /* Get request time */
    NULL,                           /* Child terminate */

    STANDARD_SAPI_MODULE_PROPERTIES
};

static int php_runtime_initialized = 0;

int php_test_runtime_init(void) {
    if (php_runtime_initialized) {
        return 0;
    }

#ifdef ZTS
    /* Initialize TSRM - based on FrankenPHP */
#if (PHP_VERSION_ID >= 80300)
    php_tsrm_startup_ex(1);
#else
    php_tsrm_startup();
#endif
#ifdef PHP_WIN32
    ZEND_TSRMLS_CACHE_UPDATE();
#endif
#endif

    /* Initialize signal handling */
    zend_signal_startup();

    /* Initialize SAPI */
    sapi_startup(&php_test_sapi_module);
    php_test_sapi_module.phpinfo_as_text = 1;

    /* Call SAPI startup (which calls php_module_startup) */
    if (php_test_sapi_module.startup(&php_test_sapi_module) == FAILURE) {
        sapi_shutdown();
#ifdef ZTS
        tsrm_shutdown();
#endif
        return -1;
    }

    /* Start request */
    if (php_request_startup() == FAILURE) {
        php_test_sapi_module.shutdown(&php_test_sapi_module);
        sapi_shutdown();
#ifdef ZTS
        tsrm_shutdown();
#endif
        return -1;
    }

    php_runtime_initialized = 1;
    return 0;
}

void php_test_runtime_shutdown(void) {
    if (!php_runtime_initialized) {
        return;
    }

    php_request_shutdown(NULL);
    php_test_sapi_module.shutdown(&php_test_sapi_module);
    sapi_shutdown();

#ifdef ZTS
    tsrm_shutdown();
#endif

    php_runtime_initialized = 0;
}
