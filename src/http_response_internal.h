/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

/* Cross-TU layout of http_response_object — shared by the PHP class
 * TU, the H/1 wire formatters and the server-side C-API TU. Kept out
 * of php_http_server.h because callers outside the tree must not see
 * the struct shape. */

#ifndef HTTP_RESPONSE_INTERNAL_H
#define HTTP_RESPONSE_INTERNAL_H

#include "php.h"
#include "zend_smart_str.h"
#include "main/php_network.h"   /* php_socket_t, SOCK_ERR */
#include "php_http_server.h"
#include "http_send_file.h"

/* Response object structure.
 * Ordered by alignment: pointers & smart_str first, then socket_fd
 * (pointer-sized on Windows), then 32-bit status_code, then bool flags
 * clustered. zend_object must stay last for PHP object layout. */
typedef struct {
    zend_string     *reason_phrase;     /* Custom reason phrase (NULL = auto) */
    HashTable       *headers;           /* Response headers (name => array of values) */
    HashTable       *trailers;          /* HTTP/2 trailers (name => value zend_string); NULL until first setTrailer */
    zend_string     *protocol_version;  /* HTTP version (e.g., "1.1") */
    smart_str        body;              /* Body buffer (pointer + size_t) */

    /* Non-owning view onto someone else's body bytes. When non-NULL,
     * holds an addref'd zend_string (typically from the persistent
     * static body cache) and the send-path emits it as a separate
     * iov entry — zero memcpy on the response side. Mutually
     * exclusive with `body`: any path that mutates the smart_str
     * must first drop this view via response_clear_body_view(). */
    zend_string     *body_view;

    /* Streaming ops + ctx. Installed by the protocol strategy at
     * dispatch; NULL for buffered-mode responses. send() activates
     * streaming by reading these; the ops interpret ctx (opaque
     * pointer to the protocol-specific stream state). */
    const http_response_stream_ops_t *stream_ops;
    void                             *stream_ctx;

    /* Connection info (for sending). SOCK_ERR if not connected. */
    php_socket_t     socket_fd;

    /* HTTP status code */
    int              status_code;

    /* State flags (clustered) */
    bool             headers_sent;
    bool             closed;
    bool             committed;
    bool             streaming;         /* send() has been called — setBody/setHeader now throw */
    bool             sse_mode;           /* SSE helpers committed the stream — send() now throws, sse* re-entry is allowed */

    /* gRPC delivery mode (grpc_mode_t), stamped by grpc_call_init_response
     * at dispatch. writeMessage / grpc_call_finish read it to pick the
     * per-frame transform (grpc-web-text base64). 0 = not a gRPC call. */
    uint8_t          grpc_mode;

    /* Compression module state (issue #8). Opaque ptr — owned by the
     * compression TU; allocated by http_compression_attach at dispatch
     * and freed by http_compression_state_free at object dtor. */
    void            *compression_state;

    /* JSON encode flags applied by ::json() when its $flags arg is 0. */
    uint32_t         default_json_flags;

    /* sendFile() handoff: when non-NULL, every mutating PHP method
     * throws and the dispose path consumes it through the per-protocol
     * sendfile FSM. Owned by the response until take_send_file pulls it. */
    http_send_file_request_t *send_file_req;

    zend_object      std;
} http_response_object;

static inline http_response_object *http_response_from_obj(zend_object *obj) {
    return (http_response_object *)((char *)(obj) - offsetof(http_response_object, std));
}

#define Z_HTTP_RESPONSE_P(zv) http_response_from_obj(Z_OBJ_P(zv))

/* Drop the borrowed-body ref if held. Call before any path that
 * mutates the smart_str body — they assume body.s is the truth. */
static inline void response_clear_body_view(http_response_object *r)
{
    if (r->body_view != NULL) {
        zend_string_release(r->body_view);
        r->body_view = NULL;
    }
}

/* Internal cross-TU accessors. Stability bounded to in-tree callers
 * — not in php_http_server.h because they expose response internals. */

zend_string                         *http_response_get_body_string(zend_object *obj);
smart_str                           *http_response_get_body_smart_str(zend_object *obj);

const http_response_stream_ops_t    *http_response_get_stream_ops(zend_object *obj);
void                                *http_response_get_stream_ctx(zend_object *obj);
void                                 http_response_replace_stream_ops(zend_object *obj,
                                          const http_response_stream_ops_t *ops,
                                          void *ctx);

void *http_response_get_compression_slot(zend_object *obj);
void  http_response_set_compression_slot(zend_object *obj, void *p);

void    http_response_set_grpc_mode(zend_object *obj, uint8_t mode);
uint8_t http_response_get_grpc_mode(zend_object *obj);

http_send_file_request_t *http_response_take_send_file(zend_object *obj);
bool                       http_response_has_send_file(zend_object *obj);

/* HTTP status reason phrases. Defined in src/http1/http1_format.c
 * alongside the pre-baked status-line table. */
const char *http_status_reason(int code);

#endif /* HTTP_RESPONSE_INTERNAL_H */
