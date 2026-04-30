/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

#ifndef HTTP_PROTOCOL_HANDLERS_H
#define HTTP_PROTOCOL_HANDLERS_H

#include "php.h"
#include "Zend/zend_async_API.h"
#include "php_http_server.h"  /* http_protocol_type_t (single source) */

/* Convert protocol type to string key for HashTable */
const char* http_protocol_type_to_string(http_protocol_type_t type);

/* Convert string to protocol type */
http_protocol_type_t http_protocol_string_to_type(const char *str);

/* Add handler to HashTable (takes ownership of fcall) */
void http_protocol_add_handler(HashTable *handlers,
                               http_protocol_type_t protocol,
                               zend_fcall_t *fcall);

/* Get handler from HashTable (returns pointer, does not copy) */
zend_fcall_t* http_protocol_get_handler(HashTable *handlers,
                                        http_protocol_type_t protocol);

/* Check if handler exists */
bool http_protocol_has_handler(HashTable *handlers, http_protocol_type_t protocol);

/* Remove handler from HashTable */
void http_protocol_remove_handler(HashTable *handlers, http_protocol_type_t protocol);

/* Internal helper for PHP methods (fluent interface) */
void http_protocol_add_handler_internal(INTERNAL_FUNCTION_PARAMETERS,
                                        HashTable *handlers,
                                        http_protocol_type_t protocol,
                                        zend_object *return_obj);

/* Initialize handlers HashTable */
void http_protocol_handlers_init(HashTable *handlers);

/* Destroy handlers HashTable */
void http_protocol_handlers_destroy(HashTable *handlers);

#endif /* HTTP_PROTOCOL_HANDLERS_H */
