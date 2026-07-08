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
#include "http_response_internal.h"   /* grpc_mode stamp on the response */

void grpc_call_init_response(zend_object *response_obj, const int grpc_mode)
{
    switch ((grpc_mode_t)grpc_mode) {
        case GRPC_MODE_WEB_TEXT:
            http_response_static_set_header(response_obj,
                "content-type", sizeof("content-type") - 1,
                GRPC_WEB_TEXT_RESPONSE_CONTENT_TYPE,
                sizeof(GRPC_WEB_TEXT_RESPONSE_CONTENT_TYPE) - 1);
            break;

        case GRPC_MODE_WEB:
            http_response_static_set_header(response_obj,
                "content-type", sizeof("content-type") - 1,
                GRPC_WEB_RESPONSE_CONTENT_TYPE,
                sizeof(GRPC_WEB_RESPONSE_CONTENT_TYPE) - 1);
            break;

        default:
            http_response_static_set_header(response_obj,
                "content-type", sizeof("content-type") - 1,
                GRPC_CONTENT_TYPE, sizeof(GRPC_CONTENT_TYPE) - 1);
            break;
    }

    http_response_set_grpc_mode(response_obj, (uint8_t)grpc_mode);
}

void grpc_call_ensure_status(zend_object *response_obj, bool had_exception)
{
    http_response_ensure_grpc_status(response_obj,
        had_exception ? GRPC_STATUS_INTERNAL : GRPC_STATUS_OK);
}

void grpc_call_finish(zend_object *response_obj,
                      const grpc_finish_ops_t *ops, void *ctx)
{
    const grpc_mode_t mode =
        (grpc_mode_t)http_response_get_grpc_mode(response_obj);

    if (mode == GRPC_MODE_WEB || mode == GRPC_MODE_WEB_TEXT) {
        /* trailers ride in-body as a 0x80 frame; clear the map so the
         * transport's EOF path doesn't emit them again */
        zend_string *frame =
            grpc_web_trailer_frame(http_response_get_trailers(response_obj));

        if (mode == GRPC_MODE_WEB_TEXT) {
            zend_string *const b64 =
                grpc_web_text_encode(ZSTR_VAL(frame), ZSTR_LEN(frame));

            zend_string_release(frame);
            frame = b64;
        }

        http_response_clear_trailers(response_obj);
        ops->append_frame_and_end(ctx, frame);
        return;
    }

    if (http_response_is_streaming(response_obj)) {
        /* native streamed: trailers ride the transport's trailer path at EOF */
        ops->end_stream(ctx);
        return;
    }

    /* zero messages → Trailers-Only: fold trailers into the initial HEADERS */
    http_response_promote_trailers_to_headers(response_obj);
    ops->commit(ctx);
}
