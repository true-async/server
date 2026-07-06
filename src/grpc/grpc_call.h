/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

#ifndef HTTP_GRPC_CALL_H
#define HTTP_GRPC_CALL_H

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include "php.h"
#include <stdbool.h>

/*
 * gRPC call lifecycle policy — the transport-independent half of gRPC
 * orchestration. The wire codec lives in grpc.c; this module owns the
 * per-call decisions (response defaults, outcome → grpc-status, which
 * finalize shape to use). Transports stay gRPC-agnostic: they classify
 * the request (cheap inline predicate), then call the two hooks below
 * and provide the tiny ops vtable for the genuinely transport-specific
 * bits (how bytes are appended / streams are ended / buffered responses
 * are committed). Native trailer delivery is NOT part of the seam — it
 * rides each transport's generic response-trailer path.
 */

/* Transport ops for grpc_call_finish. ctx is the transport's stream. */
typedef struct grpc_finish_ops {
    /* Append one body frame (consumes the zend_string ref) and end the
     * stream. Used by grpc-web to deliver the in-body trailer frame. */
    void (*append_frame_and_end)(void *ctx, zend_string *frame);

    /* End a streaming response (idempotent). Trailers are delivered by
     * the transport's generic trailer path at true EOF. */
    void (*end_stream)(void *ctx);

    /* Commit a buffered response now (single HEADERS + fin — the
     * Trailers-Only shape). May be a no-op if the connection is gone. */
    void (*commit)(void *ctx);
} grpc_finish_ops_t;

/* Dispatch-time response defaults: content-type per delivery mode (native /
 * grpc-web / grpc-web-text) + the mode stamped on the response so the
 * framing layer picks the right per-frame transform. A handler may still
 * override the content-type before its first writeMessage(). */
void grpc_call_init_response(zend_object *response_obj, int grpc_mode);

/* Outcome → grpc-status trailer. Success defaults to 0; an uncaught
 * handler exception maps to INTERNAL (13) unless the handler already set
 * a status. Call from dispose before delivery decisions. */
void grpc_call_ensure_status(zend_object *response_obj, bool had_exception);

/* Finalize delivery of a gRPC reply (dispose-time). The delivery mode is
 * read back off the response (stamped by grpc_call_init_response):
 *   grpc-web[-text] → trailers become an in-body 0x80 frame (base64-encoded
 *                     for web-text), stream ends (browsers cannot read HTTP
 *                     trailers),
 *   streaming       → end the stream; native trailers ride the transport's
 *                     generic trailer path at EOF,
 *   zero-message    → Trailers-Only: fold grpc-status/grpc-message into the
 *                     initial HEADERS and commit the buffered response. */
void grpc_call_finish(zend_object *response_obj,
                      const grpc_finish_ops_t *ops, void *ctx);

#endif /* HTTP_GRPC_CALL_H */
