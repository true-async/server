/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

#ifndef HTTP_PROTOCOL_STRATEGY_H
#define HTTP_PROTOCOL_STRATEGY_H

#include "php.h"
#include "php_http_server.h"  /* http_protocol_type_t (single source) */

typedef struct _http_connection_t http_connection_t;
typedef struct _http_protocol_strategy_t http_protocol_strategy_t;

/* http_request_t is defined in http1/http_parser.h. Forward-declare
 * it here so the callback typedef compiles without pulling llhttp in. */
typedef struct http_request_t http_request_t;

/*
 * Dispatch callback: fired once per request as soon as the protocol
 * parser decides the request is ready to be handed to the application.
 *
 * HTTP/1 invokes this from headers-complete (for streaming / upgrade
 * requests) or message-complete (fully-buffered body). HTTP/2+ invokes
 * it on HEADERS frame with END_HEADERS, once per stream.
 *
 * Set once by the connection layer right after the strategy is created.
 * The callback runs *inside* feed(), in whatever context feed() was
 * called from — currently the connection read coroutine.
 */
typedef void (*http_request_ready_cb_t)(http_connection_t *conn,
                                        http_request_t *req);

/*
 * Protocol Strategy Pattern
 *
 * One strategy instance per connection. It owns the low-level framing
 * parser (llhttp for HTTP/1, nghttp2_session for HTTP/2, etc.) and hands
 * finished requests back to the connection layer through on_request_ready.
 */
struct _http_protocol_strategy_t {
    /* Protocol identification */
    http_protocol_type_t protocol_type;
    const char *name;

    /*
     * Dispatch callback fired when a request becomes ready (headers or
     * message complete, protocol-dependent). Must be set by the
     * connection layer before the first feed() call.
     */
    http_request_ready_cb_t on_request_ready;

    /*
     * Feed freshly-read bytes into the parser.
     *
     * When the parser decides a request is ready it invokes
     * on_request_ready(conn, req) synchronously from within this call.
     *
     * consumed_out (optional): number of bytes actually consumed from
     * `data`. After a complete request the parser pauses before
     * devouring any pipelined bytes that follow, so consumed may be
     * less than len — the caller must preserve the tail for the next
     * feed.
     *
     * @return  0 = ok (progress made or message complete); the caller
     *              inspects conn->parser state / on_request_ready
     *              side effects to decide what's next,
     *         -1 = parse/framing error (connection should be closed).
     */
    int (*feed)(http_protocol_strategy_t *strategy,
                http_connection_t *conn,
                const char *data,
                size_t len,
                size_t *consumed_out);

    /*
     * Send response to client.
     *
     * @param conn     Connection context
     * @param response Response object (http_response_t*)
     */
    void (*send_response)(http_connection_t *conn, void *response);

    /*
     * Reset strategy state for next request (keep-alive).
     */
    void (*reset)(http_connection_t *conn);

    /*
     * Cleanup strategy resources.
     */
    void (*cleanup)(http_connection_t *conn);
};

/*
 * Create HTTP/1.1 strategy instance.
 * Uses llhttp for parsing.
 */
http_protocol_strategy_t* http_protocol_strategy_http1_create(void);

/*
 * Create HTTP/2 strategy instance.
 * Uses nghttp2 for parsing.
 */
http_protocol_strategy_t* http_protocol_strategy_http2_create(void);

/*
 * Destroy strategy instance.
 */
void http_protocol_strategy_destroy(http_protocol_strategy_t *strategy);

/*
 * Bitmask of protocols the server is configured to handle. Derived
 * from http_server_object::protocol_handlers at start(); detection
 * uses it to reject classifications the server has no handler for.
 */
typedef uint32_t http_protocol_mask_t;
#define HTTP_PROTO_MASK_HTTP1    (1u << HTTP_PROTOCOL_HTTP1)
#define HTTP_PROTO_MASK_HTTP2    (1u << HTTP_PROTOCOL_HTTP2)
#define HTTP_PROTO_MASK_HTTP3    (1u << HTTP_PROTOCOL_HTTP3)
#define HTTP_PROTO_MASK_WS       (1u << HTTP_PROTOCOL_WEBSOCKET)
#define HTTP_PROTO_MASK_SSE      (1u << HTTP_PROTOCOL_SSE)
#define HTTP_PROTO_MASK_GRPC     (1u << HTTP_PROTOCOL_GRPC)
#define HTTP_PROTO_MASK_ALL      (HTTP_PROTO_MASK_HTTP1 | HTTP_PROTO_MASK_HTTP2 | \
                                  HTTP_PROTO_MASK_HTTP3 | HTTP_PROTO_MASK_WS    | \
                                  HTTP_PROTO_MASK_SSE   | HTTP_PROTO_MASK_GRPC)
#define HTTP_PROTO_MASK_HAS(mask, type) (((mask) & (1u << (type))) != 0)

/*
 * Sentinel returned internally when the byte prefix matches no
 * protocol the server accepts (e.g. HTTP/2 preface on a listener with
 * h2 disabled). Routes the caller to that protocol's error path —
 * typically h2 BAD_CLIENT_MAGIC → GOAWAY — or a silent close.
 */
#define HTTP_PROTOCOL_UNSUPPORTED ((http_protocol_type_t)0xFF)

#endif /* HTTP_PROTOCOL_STRATEGY_H */
