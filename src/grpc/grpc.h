/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

#ifndef HTTP_GRPC_H
#define HTTP_GRPC_H

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include "php.h"
#include <stdbool.h>
#include <stdint.h>

/* http_request_t lives in http1/http_parser.h. Forward-declare so this
 * header stays cheap to include from dispatch / request / response TUs. */
struct http_request_t;

/* gRPC canonical status codes (grpc.github.io/grpc/core/md_doc_statuscodes).
 * Only the codes the server emits directly are named; the wire value is the
 * decimal integer carried in the `grpc-status` trailer. */
#define GRPC_STATUS_OK                    0
#define GRPC_STATUS_CANCELLED             1
#define GRPC_STATUS_UNKNOWN               2
#define GRPC_STATUS_INVALID_ARGUMENT      3
#define GRPC_STATUS_DEADLINE_EXCEEDED     4
#define GRPC_STATUS_NOT_FOUND             5
#define GRPC_STATUS_UNIMPLEMENTED        12
#define GRPC_STATUS_INTERNAL             13
#define GRPC_STATUS_UNAVAILABLE          14

/* Hard ceiling on a single inbound message length taken from the 5-byte
 * frame header — defends the deframer against a forged 4 GiB length field.
 * 16 MiB matches grpc-go's default max-receive-message-size ballpark. */
#define GRPC_MAX_RECV_MESSAGE   (16u * 1024u * 1024u)

/* Canonical gRPC content-type prefix (matches application/grpc,
 * application/grpc+proto, application/grpc;charset=..., AND the grpc-web
 * variants below, which also start with this prefix). */
#define GRPC_CONTENT_TYPE       "application/grpc"

/* Variant suffixes after the GRPC_CONTENT_TYPE prefix ("-web" also prefixes
 * "-web-text" — match web-text first) and the response content-types the
 * server emits. grpc-web carries trailers inside the response body (a
 * 0x80-flagged frame) because browsers cannot read HTTP trailers; web-text
 * additionally base64-encodes every frame independently, for clients that
 * cannot carry binary bodies (XHR). */
#define GRPC_WEB_SUFFIX       "-web"
#define GRPC_WEB_TEXT_SUFFIX  "-web-text"
#define GRPC_WEB_RESPONSE_CONTENT_TYPE      "application/grpc-web+proto"
#define GRPC_WEB_TEXT_RESPONSE_CONTENT_TYPE "application/grpc-web-text+proto"

/* Delivery mode of a gRPC call, classified once at dispatch from the request
 * content-type and stamped on the response (grpc_call_init_response). The
 * framing layer (writeMessage / grpc_call_finish) reads it back to pick the
 * per-frame transform; transports never branch on it. */
typedef enum {
    GRPC_MODE_NONE = 0,   /* not a gRPC call */
    GRPC_MODE_NATIVE,     /* application/grpc — trailers ride HTTP trailers */
    GRPC_MODE_WEB,        /* application/grpc-web — in-body 0x80 trailer frame */
    GRPC_MODE_WEB_TEXT,   /* application/grpc-web-text — WEB + per-frame base64 */
} grpc_mode_t;

/* Classify the delivery mode from the request content-type. Returns
 * GRPC_MODE_NONE for a non-gRPC request; the caller still gates on a
 * registered gRPC handler. */
grpc_mode_t grpc_request_mode(const struct http_request_t *req);

/* Convenience for the transports' buffering decision (grpc-web-text is
 * buffered by protocol nature). */
bool grpc_request_is_grpc_web_text(const struct http_request_t *req);

/* Classify-once entry point for dispatch sites: the delivery mode gated on
 * a registered addGrpcHandler — GRPC_MODE_NONE when the request is not
 * gRPC or no gRPC handler exists (the call then routes as plain HTTP).
 * Every dispatch path (H2 / H3 / worker) must classify through THIS so a
 * request can never be gRPC on one transport and plain HTTP on another. */
grpc_mode_t grpc_classify(const struct http_request_t *req, HashTable *handlers);

/* Per-frame base64 transform for grpc-web-text. Both return a new
 * zend_string the caller owns; decode returns NULL on malformed input. */
zend_string *grpc_web_text_encode(const char *in, size_t len);
zend_string *grpc_web_text_decode(const char *in, size_t len);

/* Build the grpc-web in-body trailer frame from a response trailer map:
 *   byte 0     : 0x80 (trailer frame, uncompressed)
 *   bytes 1..4 : trailer block length, uint32 big-endian
 *   bytes 5..  : `name: value\r\n` lines (HTTP/1.1 style)
 * Returns a new zend_string the caller owns. `trailers` may be NULL/empty. */
zend_string *grpc_web_trailer_frame(HashTable *trailers);

/* Parse the `grpc-timeout` request header (`<up-to-8-digits><unit>`, unit ∈
 * {H,M,S,m,u,n}) into nanoseconds. Returns 0 when absent or malformed. */
uint64_t grpc_parse_timeout_ns(const struct http_request_t *req);

/* Frame one message with the 5-byte gRPC prefix:
 *   byte 0      : compressed flag (0 = identity, 1 = compressed)
 *   bytes 1..4  : message length, uint32 big-endian
 *   bytes 5..   : message payload
 * Returns a new zend_string the caller owns. */
zend_string *grpc_frame_message(const char *msg, size_t len, bool compressed);

/* Deframe the next message from buf[*cursor .. len).
 *   returns  1 : one message extracted; *out is a new zend_string (caller
 *               owns it) and *cursor advances past the frame,
 *   returns  0 : incomplete frame — need more bytes; *cursor unchanged,
 *   returns -1 : protocol error (declared length exceeds `max_msg`).
 * `out_compressed` (nullable) receives the compressed flag. */
int grpc_deframe_next(const char *buf, size_t len, size_t *cursor,
                      uint32_t max_msg, bool *out_compressed,
                      zend_string **out);

#ifdef HAVE_HTTP_COMPRESSION
/* Decompress a message that carried the compressed flag, per the request's
 * grpc-encoding header (gzip only, via the shared compression backend).
 * Returns 0 on success (*out owned by caller), -1 on an unsupported
 * encoding or corrupt data. */
int grpc_message_inflate(const struct http_request_t *req,
                         const char *in, size_t in_len, zend_string **out);

/* Gzip-compress a message for `grpc-encoding: gzip`. Returns a new
 * zend_string the caller owns, or NULL on failure. */
zend_string *grpc_message_deflate_gzip(const char *in, size_t in_len);
#endif

#endif /* HTTP_GRPC_H */
