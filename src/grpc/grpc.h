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

/* grpc-web content-type prefix (application/grpc-web, .../grpc-web+proto,
 * .../grpc-web-text) and the response content-type the server emits. */
#define GRPC_WEB_CONTENT_TYPE_PREFIX  "application/grpc-web"
#define GRPC_WEB_RESPONSE_CONTENT_TYPE "application/grpc-web+proto"

/* grpc-web-text: the grpc-web framing, base64-encoded — the fallback for
 * transports/clients that cannot carry binary bodies (XHR). Each frame
 * (message or trailer) is base64-encoded independently with its own
 * padding, per the grpc-web protocol, so no cross-frame codec state. */
#define GRPC_WEB_TEXT_CONTENT_TYPE_PREFIX   "application/grpc-web-text"
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

/* True when the request is a gRPC call — POST with a content-type that
 * begins with `application/grpc` (this includes grpc-web). */
bool grpc_request_is_grpc(const struct http_request_t *req);

/* True when the request is a grpc-web call — content-type begins with
 * `application/grpc-web`. grpc-web carries the trailers inside the response
 * body (a 0x80-flagged frame) instead of as HTTP/2 trailers, because
 * browsers cannot read HTTP trailers. */
bool grpc_request_is_grpc_web(const struct http_request_t *req);

/* True when the request is grpc-web-text — content-type begins with
 * `application/grpc-web-text` (note: grpc_request_is_grpc_web also matches
 * these, by prefix; check web-text FIRST when distinguishing). */
bool grpc_request_is_grpc_web_text(const struct http_request_t *req);

/* Classify the delivery mode from the request content-type. Returns
 * GRPC_MODE_NONE for a non-gRPC request; the caller still gates on a
 * registered gRPC handler. */
grpc_mode_t grpc_request_mode(const struct http_request_t *req);

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

/* Decompress a message that carried the compressed flag, per the request's
 * grpc-encoding header (gzip only, via the shared compression backend).
 * Returns 0 on success (*out owned by caller), -1 on an unsupported encoding,
 * corrupt data, or when no compression backend is compiled in. */
int grpc_message_inflate(const struct http_request_t *req,
                         const char *in, size_t in_len, zend_string **out);

/* Gzip-compress a message for `grpc-encoding: gzip`. Returns a new
 * zend_string the caller owns, or NULL when gzip is unavailable / failed. */
zend_string *grpc_message_deflate_gzip(const char *in, size_t in_len);

#endif /* HTTP_GRPC_H */
