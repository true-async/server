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

/* gRPC call lifecycle policy — transport-independent; the wire codec lives
 * in grpc.c. Transports provide the ops vtable and stay gRPC-agnostic. */

/* Transport ops for grpc_call_finish. ctx is the transport's stream. */
typedef struct grpc_finish_ops {
    /* consumes the zend_string ref */
    void (*append_frame_and_end)(void *ctx, zend_string *frame);

    /* idempotent */
    void (*end_stream)(void *ctx);

    /* commit a buffered response (Trailers-Only shape) */
    void (*commit)(void *ctx);
} grpc_finish_ops_t;

/* Dispatch-time response defaults: content-type + mode stamp. */
void grpc_call_init_response(zend_object *response_obj, int grpc_mode);

/* Outcome → grpc-status; exception maps to INTERNAL unless already set. */
void grpc_call_ensure_status(zend_object *response_obj, bool had_exception);

/* Dispose-time delivery: grpc-web → in-body trailer frame + end; streaming
 * → end_stream; zero-message → Trailers-Only commit. */
void grpc_call_finish(zend_object *response_obj,
                      const grpc_finish_ops_t *ops, void *ctx);

#endif /* HTTP_GRPC_CALL_H */
