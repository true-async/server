/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

#include "grpc_call.h"
#include "grpc.h"
#include "php_http_server.h"

void grpc_call_init_response(zend_object *response_obj, bool grpc_web)
{
    if (grpc_web) {
        http_response_static_set_header(response_obj,
            "content-type", sizeof("content-type") - 1,
            GRPC_WEB_RESPONSE_CONTENT_TYPE,
            sizeof(GRPC_WEB_RESPONSE_CONTENT_TYPE) - 1);
    } else {
        http_response_static_set_header(response_obj,
            "content-type", sizeof("content-type") - 1,
            GRPC_CONTENT_TYPE, sizeof(GRPC_CONTENT_TYPE) - 1);
    }
}

void grpc_call_ensure_status(zend_object *response_obj, bool had_exception)
{
    http_response_ensure_grpc_status(response_obj,
        had_exception ? GRPC_STATUS_INTERNAL : GRPC_STATUS_OK);
}

void grpc_call_finish(zend_object *response_obj, bool grpc_web,
                      const grpc_finish_ops_t *ops, void *ctx)
{
    if (grpc_web) {
        /* Trailers ride the response body as a 0x80 frame, never as HTTP
         * trailers. Clear the trailer map so the transport's generic EOF
         * path does not also emit them as a terminal trailer block.
         * Handles both a streamed reply and a zero-message status/error
         * (the trailer frame is then the only DATA). */
        zend_string *frame =
            grpc_web_trailer_frame(http_response_get_trailers(response_obj));

        http_response_clear_trailers(response_obj);
        ops->append_frame_and_end(ctx, frame);
        return;
    }

    if (http_response_is_streaming(response_obj)) {
        /* Native gRPC over a streamed body: just make sure the stream is
         * ended; grpc-status/grpc-message ride the transport's generic
         * response-trailer path at true EOF. */
        ops->end_stream(ctx);
        return;
    }

    /* Handler streamed no messages (immediate status / error) →
     * Trailers-Only: fold grpc-status/grpc-message into the initial
     * HEADERS so the buffered commit sends a single HEADERS(:status 200,
     * fin) — the canonical gRPC shape for a bodiless response. */
    http_response_promote_trailers_to_headers(response_obj);
    ops->commit(ctx);
}
