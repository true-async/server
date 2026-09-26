/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

/* The form of a request: the fields and files of an url-encoded or multipart
 * body, keyed the way PHP fills $_POST, so `list[]`, `map[key]` and
 * `m[0][1]` nest. All three transports share it. HTTP/1 and HTTP/2 feed a
 * multipart body to its processor while the body arrives; HTTP/3 buffers the
 * body, since its transport callbacks may not write files, and the processor
 * reads it here, on first use. */

#ifndef TRUE_ASYNC_HTTP_REQUEST_FORM_H
#define TRUE_ASYNC_HTTP_REQUEST_FORM_H

#include <stdbool.h>

struct http_request_t;
struct mp_processor_t;
struct http_log_state;

/* A multipart processor for the boundary in the request's Content-Type, or
 * NULL when that header carries no usable boundary. It logs to `log_state`
 * (NULL: nowhere). The caller owns the processor; stored in
 * req->multipart_proc, it is destroyed with the request together with the temp
 * files of uploads nobody moved. */
struct mp_processor_t *http_request_form_open_multipart(const struct http_request_t *req,
														struct http_log_state *log_state);

/* Fills req->post_data and req->files from the request body, once: later calls
 * return at once. While the body is incomplete it does nothing, so the next
 * call builds the form. A request that is not a form gets two empty arrays. A
 * buffered multipart body the processor refuses gives an empty form: HTTP/1
 * and HTTP/2 refuse such a body before it is complete, HTTP/3 buffers it.
 *
 * Runs on the thread of the handler: the arrays and UploadedFile objects are
 * allocated there. A buffered multipart body is parsed in slices with a yield
 * of the current coroutine between them; false when that yield ended in an
 * exception, and no form is kept then. */
bool http_request_form_build(struct http_request_t *req);

#endif
