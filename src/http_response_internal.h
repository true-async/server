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
     * dispatch; NULL for buffered-mode responses. write() activates
     * streaming by reading these; the ops interpret ctx (opaque
     * pointer to the protocol-specific stream state). */
    const http_response_stream_ops_t *stream_ops;
    void                             *stream_ctx;

    /* The Content-Length this response committed to, or -1 when it declared
     * none. Read off the header table by the first streaming call, which is
     * also the last moment the handler can still change it, and adopted only
     * once that call's chunk is accepted. A value here binds the framing
     * (identity, not chunked) and the audit below. */
    int64_t          declared_length;

    /* Body bytes promised to the transport. Reserved before the chunk is
     * handed over, because every transport suspends inside append_chunk and a
     * second writer would otherwise read a count that is about to change. */
    uint64_t         written_length;

    /* Connection info (for sending). SOCK_ERR if not connected. */
    php_socket_t     socket_fd;

    /* HTTP status code */
    int              status_code;

    /* State flags (clustered) */
    bool             headers_sent;
    bool             closed;
    bool             aborted;           /* abort(): finished as failed; implies `closed` */
    bool             committed;
    bool             streaming;         /* write() has been called — setBody/setHeader now throw */
    bool             sse_mode;           /* SSE helpers committed the stream — write() now throws, sse* re-entry is allowed */
    bool             is_head;            /* HEAD: write() drops chunks (RFC 9110 §9.3.2) */
    bool             handler_wants_close; /* setHeader('Connection', 'close'): the field is
                                           * the server's to emit, so the handler's is taken
                                           * as the request it is and answered by closing the
                                           * connection, not by copying the bytes. */

    /* grpc_mode_t stamped at dispatch; picks the per-frame transform.
     * 0 = not a gRPC call. */
    uint8_t          grpc_mode;

    bool             grpc_compress;   /* setGrpcEncoding('gzip') declared */

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

/* RFC 9112 §6.3 rule 1: a 1xx, 204 or 304 response ends at the blank line
 * whatever its headers say. Such a response carries no body to frame, so a
 * Content-Length on it describes something other than what follows — the
 * representation a 304 stands for, say — and the server neither audits it nor
 * replaces it. */
static inline bool response_status_carries_body(const int status)
{
    return status >= 200 && status != 204 && status != 304;
}

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

/* Replace the reason phrase with what RFC 9112 §4 allows on a status line:
 * HTAB, SP, VCHAR and obs-text. Every other byte becomes a space, because the
 * value reaches this field from handler data — an uncaught exception's message
 * is the usual route — and a CR or an LF in it ends the status line, so the
 * bytes behind it are read as headers and as a second response. */
void http_response_set_reason_phrase(zend_object *obj, const char *phrase, size_t len);

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
