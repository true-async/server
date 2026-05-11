/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

/* HTTP/2 static-file body delivery — protocol-owned half of the
 * static handler. Mirror of src/http1/http1_sendfile.c. The static
 * handler resolves the path, opens the file, decides 200/206/304/HEAD,
 * populates response_obj, then hands ownership here via the
 * send_static_response vtable method.
 *
 * This module:
 *   - Flattens response_obj headers into nghttp2_nv[] (filtering H2-
 *     forbidden Connection / Keep-Alive / Transfer-Encoding / Upgrade /
 *     Content-Length per RFC 9113 §8.2.2 — Content-Length is implicit
 *     in DATA frames).
 *   - For HEAD / 304 / inline-body errors: nghttp2_submit_response with
 *     no data_provider OR with the response_obj inline body buffer.
 *   - For 200 / 206: registers a data_provider that pread()s the body
 *     slice from file_io->descriptor.fd. The static handler did the
 *     async OPEN — once we own the fd, pread is fine (file IO won't
 *     block the loop the way socket IO would).
 *   - Disposes file_io via the stream's on_close hook so on_done fires
 *     after the wire bytes really drained. */

#ifndef HTTP2_STATIC_RESPONSE_H
#define HTTP2_STATIC_RESPONSE_H

#include "php_http_server.h"

/* Vtable entry exposed via h2_stream_ops.send_static_response. ctx is
 * the http2_stream_t* the strategy installed at dispatch. See
 * http_response_stream_ops_t::send_static_response for the contract. */
int h2_stream_send_static_response(void *ctx,
                                   zend_object *response_obj,
                                   zend_async_io_t *file_io,
                                   uint64_t body_offset,
                                   uint64_t body_length,
                                   bool head_only,
                                   void (*on_done)(void *user, int status),
                                   void *user);

#endif /* HTTP2_STATIC_RESPONSE_H */
