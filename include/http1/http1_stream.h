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

#endif /* HTTP1_STREAM_H */
