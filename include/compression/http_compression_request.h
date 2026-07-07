/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

/*
 * Inbound request body decoding (Content-Encoding: gzip from clients).
 * Phase 1: gzip only. Unknown codings → 415. Bomb-cap exceeded → 413.
 *
 * Caller owns the request struct; on success, req->body is replaced
 * with the decoded zend_string and the original is released. The
 * Content-Encoding header is left in place — callers that round-trip
 * the request elsewhere keep the wire-truth intact; the decoded
 * body is what handlers see, and that is what matters at the API.
 */
#ifndef HTTP_COMPRESSION_REQUEST_H
#define HTTP_COMPRESSION_REQUEST_H

#include "php.h"   /* zend_string */

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    HTTP_DECODE_OK             = 0,    /* no coding, identity, or successful inflate */
    HTTP_DECODE_UNKNOWN_CODING = 415,  /* coding the server does not implement */
    HTTP_DECODE_TOO_LARGE      = 413,  /* exceeded request_max_decompressed_size */
    HTTP_DECODE_MALFORMED      = 400,  /* zlib reported corruption */
} http_decode_status_t;

typedef struct http_request_t        http_request_t;
typedef struct _http_server_config_t http_server_config_t;

/* Decode req->body in place. Returns one of HTTP_DECODE_*. The numeric
 * value of every non-OK return is the HTTP status the caller should
 * emit — keeps the call site free of mapping tables. */
int http_compression_decode_request_body(http_request_t *req,
                                         http_server_config_t *cfg);

/* Per-codec request decoders. Defined in their respective backend TUs;
 * declared here so http_compression_request.c can dispatch without
 * pulling in the codec-specific headers (libbrotli / libzstd). The
 * dispatcher only calls these when the matching HAVE_HTTP_* macro is
 * defined, so unbuilt codecs do not need stub implementations. */
int http_compression_decode_request_brotli(http_request_t *req, size_t cap);
int http_compression_decode_request_zstd(http_request_t *req, size_t cap);

/* One-shot whole-buffer gzip helpers for message-level compression (gRPC
 * per-message frames), reusing the same zlib backend as the body paths.
 *
 * deflate: compress `in` as a standalone gzip member; returns a new
 * zend_string the caller owns, or NULL on failure. `level` clamped to 1..9.
 * Defined in http_compression_gzip.c.
 *
 * inflate: returns 0 on success (*out set, caller owns it), -1 on a
 * malformed stream, -2 when the output would exceed `max_out`
 * (max_out == 0 → unbounded). Shares the inflate loop + zip-bomb guard
 * with the request-body decoder in this TU. */
zend_string *http_compression_gzip_deflate_buffer(const char *in, size_t in_len,
                                                  int level);
int http_compression_gzip_inflate_buffer(const char *in, size_t in_len,
                                         size_t max_out, zend_string **out);

#ifdef __cplusplus
}
#endif

#endif /* HTTP_COMPRESSION_REQUEST_H */
