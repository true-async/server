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
 * application/grpc+proto, application/grpc;charset=..., etc.). */
#define GRPC_CONTENT_TYPE       "application/grpc"

/* True when the request is a gRPC call — POST with a content-type that
 * begins with `application/grpc`. */
bool grpc_request_is_grpc(const struct http_request_t *req);

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

#endif /* HTTP_GRPC_H */
