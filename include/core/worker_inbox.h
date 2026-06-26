/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

#ifndef WORKER_INBOX_H
#define WORKER_INBOX_H

#include "php.h"
#include "Zend/zend_async_API.h"
#include "core/worker_dispatch.h"            /* http_request_t, worker_response_sink_fn */

/*
 * Worker inbox for the reactor/worker split (issue #80, B2/D7).
 *
 * The consumer side of the request handoff: a per-worker #81 mailbox
 * (thread_mailbox) whose drain runs on the worker's reactor thread and feeds
 * each posted http_request_t to worker_dispatch_request (B1b/D7) — wrap +
 * spawn handler coroutine + render the response to the sink. The reactor (any
 * thread) builds the request and posts its POINTER; the worker drains and
 * dispatches on its own thread, so business logic never runs on the transport
 * reactor.
 *
 * One inbox per worker. The registry that lets a reactor pick which worker's
 * inbox to post to is the producer side, wired in B3.
 *
 * Threading contract:
 *   - worker_inbox_create()/free() run on the worker (consumer) thread; they
 *     create/dispose the underlying libuv-backed mailbox on that loop.
 *   - worker_inbox_post() runs on any thread (typically a reactor).
 *   - free() must run after producers have quiesced.
 */

typedef struct worker_inbox_s worker_inbox_t;

/* Create a worker inbox on THIS (worker) thread. Dispatched requests run their
 * handler in `scope` (own_scope mirrors worker_dispatch_request) against
 * `server`'s handler table; each rendered response goes to `sink`. Returns NULL
 * if no reactor is running on the calling thread or on allocation failure. */
worker_inbox_t *worker_inbox_create(http_server_object *server,
                                    zend_async_scope_t *scope,
                                    bool own_scope,
                                    worker_response_sink_fn sink, void *sink_arg);

/* Post a request to the inbox (any thread). Ownership of `req` transfers to the
 * inbox — the worker dispatch path becomes its sole owner and frees it via the
 * request lifecycle. Returns false if the bounded mailbox is full (backpressure;
 * the caller keeps ownership) or on bad arguments. */
bool worker_inbox_post(worker_inbox_t *inbox, http_request_t *req);

/* Approximate queued request count (producer backpressure visibility). */
size_t worker_inbox_depth(const worker_inbox_t *inbox);

/* Tear down the inbox. Consumer-thread only; producers must have quiesced. */
void worker_inbox_free(worker_inbox_t *inbox);

#endif /* WORKER_INBOX_H */
