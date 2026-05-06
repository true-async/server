/*
 * Response-side compression: state attached to HttpResponse, the
 * buffered-apply hook, the streaming-ops wrapper, and the per-response
 * opt-out flag. All decisions go through one decide() that combines
 * request, response and config inputs — single source of truth.
 *
 * Lifetime: the state struct is allocated lazily by http_compression_attach
 * (called by each protocol dispatch right after install_stream_ops) and
 * freed by http_compression_state_free at object dtor.
 */
#ifndef HTTP_COMPRESSION_RESPONSE_H
#define HTTP_COMPRESSION_RESPONSE_H

#include <stdbool.h>
#include <stddef.h>

#include "compression/http_encoder.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Forward decls — kept here so callers don't need to include the full
 * Zend / php_http_server.h in inline-light call sites. */
typedef struct http_request_t          http_request_t;
typedef struct _http_server_config_t   http_server_config_t;
struct _zend_object;

/* Allocate compression state on the response and remember the request
 * + server config it was dispatched with. The request is held by a
 * non-owning pointer — the dispatch ctx already keeps it alive for as
 * long as the response zval is. No-op when compression is disabled in
 * cfg (state stays NULL, hooks below cheaply early-return). */
void http_compression_attach(struct _zend_object *response_obj,
                             http_request_t *request,
                             http_server_config_t *cfg);

/* Free state attached above. Called from the response object's free_obj. */
void http_compression_state_free(struct _zend_object *response_obj);

/* Mark this response as ineligible for compression (BREACH-sensitive
 * endpoints, handler-controlled binary blobs, etc.). Idempotent. */
void http_compression_mark_no_compression(struct _zend_object *response_obj);

/* Buffered hook: when the response is being serialised (called from
 * http_response_format / format_parts), gzip the body in place and
 * mutate headers (Content-Encoding, Vary, drop Content-Length).
 * Idempotent — a second call is a no-op. Cheap fast path when state
 * is NULL or decide() returns identity. */
void http_compression_apply_buffered(struct _zend_object *response_obj);

/* Streaming hook: at the first HttpResponse::send() call, swap the
 * installed stream_ops with a compressing wrapper if decide() says yes.
 * Mutates response headers in the same shot so the H1 chunked emitter
 * commits them with Content-Encoding already set. Cheap no-op when
 * state is NULL or decide() returns identity. */
void http_compression_maybe_install_stream_wrapper(
    struct _zend_object *response_obj);

#ifdef __cplusplus
}
#endif

#endif /* HTTP_COMPRESSION_RESPONSE_H */
