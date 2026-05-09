/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

/* Cross-TU contract for http_response_object accessors that aren't
 * part of the public PHP-facing API. Used by protocol layers
 * (h1/h2/h3/static/compression) that need direct read access to the
 * response structure to serialize the wire bytes themselves —
 * bypassing http_response_format which would auto-add Content-Length
 * and run the compression hook.
 *
 * These are NOT part of php_http_server.h because they expose
 * internal storage; their stability is bounded to in-tree callers. */

#ifndef HTTP_RESPONSE_INTERNAL_H
#define HTTP_RESPONSE_INTERNAL_H

#include "php.h"
#include "php_http_server.h"

int                                  http_response_get_status_code(zend_object *obj);
HashTable                           *http_response_get_headers_table(zend_object *obj);
zend_string                         *http_response_get_body_string(zend_object *obj);

const http_response_stream_ops_t    *http_response_get_stream_ops(zend_object *obj);
void                                *http_response_get_stream_ctx(zend_object *obj);
void                                 http_response_replace_stream_ops(zend_object *obj,
                                          const http_response_stream_ops_t *ops,
                                          void *ctx);

/* Compression-slot accessors. Used by the compression module. */
void *http_response_get_compression_slot(zend_object *obj);
void  http_response_set_compression_slot(zend_object *obj, void *p);

/* Aliases used by the compression module pre-dating the get/set
 * naming above. Keep matching the existing extern names there. */
HashTable *http_response_get_headers(zend_object *obj);
int        http_response_get_status(zend_object *obj);

/* Pending sendFile descriptor accessor (issue #13). After
 * HttpResponse::sendFile(), a snapshot of the path + options sits on
 * the response object until the dispose path takes ownership and
 * dispatches it through the per-protocol FSM. */
#include "http_send_file.h"
http_send_file_request_t *http_response_take_send_file(zend_object *obj);
bool                       http_response_has_send_file(zend_object *obj);

#endif /* HTTP_RESPONSE_INTERNAL_H */
