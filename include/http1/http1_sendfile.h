/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

/* HTTP/1.1 static-file body delivery — protocol-owned half of the
 * static handler. The static handler resolves the path, opens the
 * file, decides 200/206/304/HEAD/error, populates response_obj, then
 * hands ownership to this module via the send_static_response vtable
 * method. From here the module:
 *
 *   - serializes status line + headers off response_obj
 *   - submits the head (plain TCP fire-and-forget OR TLS atomic
 *     SSL_write through the BIO ring)
 *   - if a body source was provided: kernel sendfile (plain TCP) OR
 *     chunked IO_READ → SSL_write → drain loop (TLS)
 *   - disposes file_io, fires the on_done callback, frees its state
 *
 * The static handler keeps the lifecycle (counters, http_request_
 * finalize, on_missing:Next rollback). This module is purely
 * bytes-on-the-wire for one in-flight static request. */

#ifndef HTTP1_SENDFILE_H
#define HTTP1_SENDFILE_H

#include "php_http_server.h"

/* Vtable entry exposed via h1_stream_ops.send_static_response. ctx is
 * the http1_request_ctx_t* the strategy installed at dispatch. See
 * http_response_stream_ops_t::send_static_response for the contract. */
int h1_stream_send_static_response(void *ctx,
                                   zend_object *response_obj,
                                   zend_async_io_t *file_io,
                                   uint64_t body_offset,
                                   uint64_t body_length,
                                   bool head_only,
                                   void (*on_done)(void *user, int status),
                                   void *user);

#endif /* HTTP1_SENDFILE_H */
