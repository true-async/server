/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

#ifndef WORKER_DISPATCH_H
#define WORKER_DISPATCH_H

#include "php.h"
#include "Zend/zend_async_API.h"
#include "core/response_wire.h"
#include "http1/http_parser.h"               /* http_request_t */

/*
 * Worker-side request dispatch for the reactor/worker split (issue #80, B1b/D7).
 *
 * The transport reactor builds the request directly into a persistent
 * http_request_t (D7) and hands the POINTER to a PHP worker (actor handoff —
 * no copy-marshal). THIS is the worker side: it wraps the request in an
 * HttpRequest zval on its own thread, spawns the user handler coroutine (so
 * business logic runs off the transport thread), and when the handler finishes
 * renders the HttpResponse into a flat response_wire (D3) handed back to a
 * sink — which posts it to the reactor for nghttp3 encode + send.
 *
 * Everything here runs on the worker thread: the request/response zvals and the
 * handler coroutine never touch the reactor. The request crosses the thread
 * boundary by pointer (the worker becomes its sole owner); the response crosses
 * back as a flat response_wire.
 */

typedef struct http_server_object http_server_object;

/* Sink for response wires, worker thread. Owns `rw` in every outcome.
 * Returns false on definitive delivery failure — the caller must then fail
 * the stream (STREAM_* fragments must never be dropped silently). */
typedef bool (*worker_response_sink_fn)(response_wire_t *rw, void *sink_arg);

/* Discard an undeliverable wire: abandon its credit (marks the producer's
 * stream dead), release the owned chunk, free the wire. Zend-side companion
 * to response_wire_free — every drop site must go through this so a new
 * owned field can't leak from a forgotten copy. */
void response_wire_discard(response_wire_t *rw);

/* Take ownership of `req` (a persistent reactor-built or ZMM request, refcount
 * 1), wrap it in an HttpRequest on THIS (worker) thread, spawn the user handler
 * coroutine in `scope`, and when it finishes render the HttpResponse into a
 * response_wire (echoing the request's reactor_id / stream_id / conn) handed to
 * `sink`.
 *
 * Ownership: `req` is consumed unconditionally — on success the HttpRequest
 * object owns it (freed via http_request_destroy when the coroutine disposes);
 * on every failure path this function destroys it before returning. The caller
 * must not touch or free `req` after the call.
 *
 * `own_scope` mirrors the H3 dispatch flag: true gives each request its own
 * request_context() subtree (a child of `scope`); false runs directly in
 * `scope`. When no handler is registered a 404 is synthesised so the sink still
 * fires. Buffered responses go out as one FULL wire at dispose; a streaming
 * response (send()/writeMessage()/SSE) is marshalled incrementally as
 * STREAM_HEADERS / STREAM_CHUNK / STREAM_END wires, paced by the per-stream
 * credit block (stream_credit.h) the reactor acknowledges against.
 *
 * Returns true once the handler coroutine is enqueued; false on hard failure
 * (bad args / allocation / no current coroutine to spawn under), in which case
 * the sink is not called. Requires an active TrueAsync scheduler on the calling
 * thread. */
bool worker_dispatch_request(http_server_object *server,
                             zend_async_scope_t *scope,
                             http_request_t *req,
                             bool own_scope,
                             worker_response_sink_fn sink, void *sink_arg);

#endif /* WORKER_DISPATCH_H */
