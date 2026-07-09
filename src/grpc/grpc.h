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

struct http_request_t;

/* canonical status codes — decimal value of the grpc-status trailer */
#define GRPC_STATUS_OK                    0
#define GRPC_STATUS_CANCELLED             1
#define GRPC_STATUS_UNKNOWN               2
#define GRPC_STATUS_INVALID_ARGUMENT      3
#define GRPC_STATUS_DEADLINE_EXCEEDED     4
#define GRPC_STATUS_NOT_FOUND             5
#define GRPC_STATUS_UNIMPLEMENTED        12
#define GRPC_STATUS_INTERNAL             13
#define GRPC_STATUS_UNAVAILABLE          14

/* inbound message ceiling — the frame-header length is attacker-controlled */
#define GRPC_MAX_RECV_MESSAGE   (16u * 1024u * 1024u)

#define GRPC_CONTENT_TYPE       "application/grpc"

/* "-web" also prefixes "-web-text" — match web-text first */
#define GRPC_WEB_SUFFIX       "-web"
#define GRPC_WEB_TEXT_SUFFIX  "-web-text"
#define GRPC_WEB_RESPONSE_CONTENT_TYPE      "application/grpc-web+proto"
#define GRPC_WEB_TEXT_RESPONSE_CONTENT_TYPE "application/grpc-web-text+proto"

/* Classified once at dispatch, stamped on the response; transports never
 * branch on it. */
typedef enum {
    GRPC_MODE_NONE = 0,   /* not a gRPC call */
    GRPC_MODE_NATIVE,     /* application/grpc — trailers ride HTTP trailers */
    GRPC_MODE_WEB,        /* application/grpc-web — in-body 0x80 trailer frame */
    GRPC_MODE_WEB_TEXT,   /* application/grpc-web-text — WEB + per-frame base64 */
} grpc_mode_t;

/* Delivery mode from the request content-type; NONE for non-gRPC. Called
 * only by http_request_classify_protocols — everyone else reads the
 * req->grpc_mode stamp. */
grpc_mode_t grpc_request_mode(const struct http_request_t *req);

/* Stamped mode gated on a registered gRPC handler; every dispatch path
 * (H2 / H3 / worker) must classify through this. */
grpc_mode_t grpc_classify(const struct http_request_t *req, HashTable *handlers);

/* New string; decode returns NULL on malformed input. */
zend_string *grpc_web_text_encode(const char *in, size_t len);
zend_string *grpc_web_text_decode(const char *in, size_t len);

/* In-body trailer frame: 0x80, u32be length, "name: value\r\n" lines. */
zend_string *grpc_web_trailer_frame(HashTable *trailers);

/* grpc-timeout header → ns; 0 when absent or malformed. */
uint64_t grpc_parse_timeout_ns(const struct http_request_t *req);

/* 5-byte prefix (compressed flag + u32be length) + payload. */
zend_string *grpc_frame_message(const char *msg, size_t len, bool compressed);

/* 1 = *out extracted (caller owns), *cursor advanced; 0 = need more bytes;
 * -1 = declared length exceeds max_msg. */
int grpc_deframe_next(const char *buf, size_t len, size_t *cursor,
                      uint32_t max_msg, bool *out_compressed,
                      zend_string **out);

#ifdef HAVE_HTTP_COMPRESSION
/* Per the request's grpc-encoding (gzip only). 0 = ok, -1 = unsupported/corrupt. */
int grpc_message_inflate(const struct http_request_t *req,
                         const char *in, size_t in_len, zend_string **out);

/* Gzip-compress for `grpc-encoding: gzip`; NULL on failure. */
zend_string *grpc_message_deflate_gzip(const char *in, size_t in_len);
#endif

#endif /* HTTP_GRPC_H */
