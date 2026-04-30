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
#include "http_protocol_handlers.h"

/* {{{ http_protocol_type_to_string */
const char* http_protocol_type_to_string(http_protocol_type_t type)
{
    switch (type) {
        case HTTP_PROTOCOL_HTTP1:     return "http";
        case HTTP_PROTOCOL_HTTP2:     return "http2";
        case HTTP_PROTOCOL_HTTP3:     return "http3";
        case HTTP_PROTOCOL_WEBSOCKET: return "websocket";
        case HTTP_PROTOCOL_SSE:       return "sse";
        case HTTP_PROTOCOL_GRPC:      return "grpc";
        case HTTP_PROTOCOL_UNKNOWN:   return "unknown";
    }
    return "unknown";
}
/* }}} */

/* {{{ http_protocol_string_to_type */
http_protocol_type_t http_protocol_string_to_type(const char *str)
{
    if (strcmp(str, "http") == 0)      return HTTP_PROTOCOL_HTTP1;
    if (strcmp(str, "http2") == 0)     return HTTP_PROTOCOL_HTTP2;
    if (strcmp(str, "http3") == 0)     return HTTP_PROTOCOL_HTTP3;
    if (strcmp(str, "websocket") == 0) return HTTP_PROTOCOL_WEBSOCKET;
    if (strcmp(str, "sse") == 0)       return HTTP_PROTOCOL_SSE;
    if (strcmp(str, "grpc") == 0)      return HTTP_PROTOCOL_GRPC;
    return HTTP_PROTOCOL_UNKNOWN;
}
/* }}} */

/* {{{ handler_entry_dtor - destructor for HashTable entries */
static void handler_entry_dtor(zval *zv)
{
    zend_fcall_t *fcall = (zend_fcall_t *)Z_PTR_P(zv);
    if (fcall) {
        /* Release function name reference */
        zval_ptr_dtor(&fcall->fci.function_name);
        efree(fcall);
    }
}
/* }}} */

/* {{{ http_protocol_handlers_init */
void http_protocol_handlers_init(HashTable *handlers)
{
    zend_hash_init(handlers, 8, NULL, handler_entry_dtor, 0);
}
/* }}} */

/* {{{ http_protocol_handlers_destroy */
void http_protocol_handlers_destroy(HashTable *handlers)
{
    zend_hash_destroy(handlers);
}
/* }}} */

/* {{{ http_protocol_add_handler */
void http_protocol_add_handler(HashTable *handlers,
                               http_protocol_type_t protocol,
                               zend_fcall_t *fcall)
{
    if (!handlers || !fcall) {
        return;
    }

    const char *protocol_str = http_protocol_type_to_string(protocol);

    /* Keep reference to function name */
    Z_TRY_ADDREF(fcall->fci.function_name);

    /* Add to HashTable (will call dtor if key exists) */
    zval entry_zv;
    ZVAL_PTR(&entry_zv, fcall);
    zend_hash_str_update(handlers, protocol_str, strlen(protocol_str), &entry_zv);
}
/* }}} */

/* {{{ http_protocol_get_handler */
zend_fcall_t* http_protocol_get_handler(HashTable *handlers,
                                        http_protocol_type_t protocol)
{
    if (!handlers) {
        return NULL;
    }

    const char *protocol_str = http_protocol_type_to_string(protocol);
    zval *entry_zv = zend_hash_str_find(handlers, protocol_str, strlen(protocol_str));

    if (!entry_zv || Z_TYPE_P(entry_zv) != IS_PTR) {
        return NULL;
    }

    return (zend_fcall_t *)Z_PTR_P(entry_zv);
}
/* }}} */

/* {{{ http_protocol_has_handler */
bool http_protocol_has_handler(HashTable *handlers, http_protocol_type_t protocol)
{
    if (!handlers) {
        return false;
    }

    const char *protocol_str = http_protocol_type_to_string(protocol);
    return zend_hash_str_exists(handlers, protocol_str, strlen(protocol_str));
}
/* }}} */

/* {{{ http_protocol_remove_handler */
void http_protocol_remove_handler(HashTable *handlers, http_protocol_type_t protocol)
{
    if (!handlers) {
        return;
    }

    const char *protocol_str = http_protocol_type_to_string(protocol);
    zend_hash_str_del(handlers, protocol_str, strlen(protocol_str));
}
/* }}} */

/* {{{ http_protocol_add_handler_internal - helper for PHP methods */
void http_protocol_add_handler_internal(INTERNAL_FUNCTION_PARAMETERS,
                                        HashTable *handlers,
                                        http_protocol_type_t protocol,
                                        zend_object *return_obj)
{
    zend_fcall_info fci;
    zend_fcall_info_cache fcc;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_FUNC(fci, fcc)
    ZEND_PARSE_PARAMETERS_END();

    /* Allocate zend_fcall_t */
    zend_fcall_t *fcall = ecalloc(1, sizeof(zend_fcall_t));
    fcall->fci = fci;
    fcall->fci_cache = fcc;

    /* Add handler to HashTable */
    http_protocol_add_handler(handlers, protocol, fcall);

    if (UNEXPECTED(EG(exception))) {
        efree(fcall);
        return;
    }

    /* Return $this for fluent interface */
    RETURN_OBJ_COPY(return_obj);
}
/* }}} */
