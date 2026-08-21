/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

#ifndef HTTP1_STREAM_H
#define HTTP1_STREAM_H

#include "php_http_server.h"

/* HTTP/1.1 chunked-encoding streaming vtable.
 * Context is an http_connection_t*. One response in flight per TCP,
 * no multiplexing — the ops write directly to the socket via
 * http_connection_send, so no internal queue is needed. Kernel
 * send-buffer backpressure is the natural pushback mechanism.
 *
 * Installed by http_connection_dispatch_request for HTTP/1 responses;
 * send() on the response activates chunked mode on first call
 * (commits status line + headers with Transfer-Encoding: chunked). */
extern const http_response_stream_ops_t h1_stream_ops;

/* How an HTTP/1 response body is delimited on the wire. The answer comes from
 * the request and the response together — the version the peer speaks, the
 * method it used, the status the handler chose, the length it declared — and
 * it is fixed once, when the header block is built, because every byte after
 * that is framed to match.
 *
 * These are the four a server can produce out of the eight cases RFC 9112 §6.3
 * lists for a receiver. */
typedef enum {
    /* Rule 1: a 1xx, 204 or 304, and any response to HEAD. The message ends at
     * the blank line whatever the header fields say, so nothing is framed and
     * no body goes out. A HEAD still carries the length its GET would have
     * stated, which the buffered formatter computes. */
    H1_FRAMING_NONE = 0,
    /* Rule 5: Content-Length, and the body is exactly that many bytes. */
    H1_FRAMING_LENGTH,
    /* Rule 3: Transfer-Encoding: chunked. Needs a peer that speaks HTTP/1.1 —
     * §6.1 forbids sending it to one that indicated 1.0. */
    H1_FRAMING_CHUNKED,
    /* Rule 7: no boundary but the connection close. The answer for a 1.0 peer
     * whose body length is not known in advance; it costs the connection, so
     * nothing may follow such a response on it. */
    H1_FRAMING_CLOSE,
} h1_framing_t;

/* The framing @p response_obj will get on the streaming path. Safe to call
 * before the header block is built and after it: the four inputs are all fixed
 * by the first streaming call. The buffered formatter reads the same inputs
 * directly, because it also has a body length of its own to state. */
h1_framing_t h1_response_framing(zend_object *response_obj);

/* Whether the request this response answers indicated HTTP/1.1 or later, which
 * decides what the connection may be told and how the body may be framed
 * (RFC 9112 §6.1, §9.3). A response built outside a connection carries no
 * version and counts as 1.1, which is what every modern peer reads. */
bool h1_response_peer_speaks_http11(zend_object *response_obj);

#endif /* HTTP1_STREAM_H */
