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

/* The one shared handler precedence for every dispatch site: gRPC (when
 * classified) → @p protocol → HTTP1 → HTTP2. NULL when nothing matches.
 *
 * @p protocol is the transport the request actually arrived on, so a
 * registration made for it wins over the general one — otherwise
 * addHttp2Handler is unreachable on any server that also calls
 * addHttpHandler. The HTTP1 step behind it is the general registration: most
 * servers make only that one, and it has always answered every protocol. */
zend_fcall_t *http_protocol_pick_handler(HashTable *handlers,
                                         http_protocol_type_t protocol,
                                         bool is_grpc);

/* Stamp the request's grpc_mode and form_kind once at headers-complete,
 * before any body-streaming decision; transports never classify themselves
 * and read the body policy through the predicates below. */
void http_request_classify_protocols(struct http_request_t *req);

/* Shared preparation before a PHP handler or static handler sees the request
 * and response. Use the dispatch thread's config (the worker's clone in pool
 * mode). Trace context may already have been parsed at headers-complete for
 * WebSocket upgrades, which bypass the normal HTTP dispatch. */
void http_request_prepare_dispatch(struct http_request_t *req,
                                   zend_object *response,
                                   http_server_config_t *config,
                                   bool telemetry_enabled);

/* Body policy derived from the stamped grpc_mode and form_kind, keeping
 * transports gRPC-agnostic. must_buffer: never stream (grpc-web-text decodes
 * the whole body, and a form getter reads the whole body). size_uncapped:
 * waive the cumulative max_body_size cap (gRPC streams are unbounded; the
 * in-flight window still bounds memory). */
bool http_request_body_must_buffer(const struct http_request_t *req);
bool http_request_body_size_uncapped(const struct http_request_t *req);

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
