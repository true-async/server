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

#ifdef __cplusplus
}
#endif

#endif /* HTTP_COMPRESSION_REQUEST_H */
