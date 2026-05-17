/*
 * +----------------------------------------------------------------------+
 * | TrueAsync HTTP Server                                                |
 * +----------------------------------------------------------------------+
 *
 * Streaming request body (issue #26).
 *
 * Per-request chunk queue + wakeup event. Producers are the protocol
 * parsers (H1 on_body, H2 cb_on_data_chunk_recv, H3 on_recv_data) and
 * the sole consumer is the handler coroutine via HttpRequest::readBody().
 *
 * No locking — producer and consumer run on the same reactor thread.
 */

#ifndef HTTP_BODY_STREAM_H
#define HTTP_BODY_STREAM_H

#include "http1/http_parser.h"   /* http_request_t, http_body_chunk_t */

/* 3-case streaming policy (issue #26):
 *   CL <  SMALL              never stream — wait body_event, return whole
 *   SMALL <= CL < AUTO       buffer; upgrade on first readBody()
 *   CL >= AUTO or unknown    stream from on_headers_complete            */
#define HTTP_BODY_STREAM_THRESHOLD      (64u * 1024u)
#define HTTP_BODY_STREAM_AUTO_THRESHOLD (1u * 1024u * 1024u)

/* Push one chunk to the queue. Takes a ref on `data` (caller may
 * release after return). Lazy-creates body_data_event on first push.
 * Returns false on alloc failure (req->body_error is set). */
bool http_body_stream_push(http_request_t *req, zend_string *data);

/* Mark end-of-stream. Idempotent. Fires body_data_event so a parked
 * readBody() wakes and returns null. */
void http_body_stream_close(http_request_t *req);

/* Mark hard error (connection drop, max_body_size exceeded). Sets
 * body_error + body_eof and fires the wakeup event. */
void http_body_stream_error(http_request_t *req);

/* Pop one chunk from the queue. Returns NULL when queue is empty
 * (caller checks body_eof). Caller owns returned ref. */
zend_string *http_body_stream_pop(http_request_t *req);

/* Free the queue (releases all pending chunk refs) and dispose
 * body_data_event. Called from http_request_destroy. */
void http_body_stream_dispose(http_request_t *req);

#endif /* HTTP_BODY_STREAM_H */
