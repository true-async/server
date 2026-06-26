/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+

  Worker inbox (#80, B2) — per-worker request mailbox + dispatch drain.
  See include/core/worker_inbox.h.
*/

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include "core/worker_inbox.h"
#include "core/thread_mailbox.h"

/* Bounded so a backed-up worker backpressures the reactor (post returns false)
 * rather than growing the queue without limit. */
#define WORKER_INBOX_CAPACITY 1024
#define WORKER_INBOX_BATCH      64

struct worker_inbox_s {
    thread_mailbox_t        *mb;
    http_server_object      *server;
    zend_async_scope_t      *scope;
    worker_response_sink_fn  sink;
    void                    *sink_arg;
    bool                     own_scope;
};

/* Runs on the worker's reactor thread when requests are queued. Each item is an
 * http_request_t whose ownership passed to us at post: hand it to dispatch (the
 * handler coroutine renders the response to the sink). worker_dispatch_request
 * consumes the request unconditionally — owns it on success, destroys it on
 * failure — so the drain never frees it here. */
static void worker_inbox_drain(void **items, const size_t count, void *arg)
{
    worker_inbox_t *const inbox = (worker_inbox_t *)arg;

    for (size_t i = 0; i < count; i++) {
        http_request_t *const req = (http_request_t *)items[i];

        if (UNEXPECTED(req == NULL)) {
            continue;
        }

        worker_dispatch_request(inbox->server, inbox->scope, req,
                                inbox->own_scope, inbox->sink, inbox->sink_arg);
    }
}

worker_inbox_t *worker_inbox_create(http_server_object *server,
                                    zend_async_scope_t *scope,
                                    const bool own_scope,
                                    worker_response_sink_fn sink, void *sink_arg)
{
    if (UNEXPECTED(server == NULL || scope == NULL)) {
        return NULL;
    }

    worker_inbox_t *const inbox = pecalloc(1, sizeof(*inbox), 0);
    inbox->server    = server;
    inbox->scope     = scope;
    inbox->own_scope = own_scope;
    inbox->sink      = sink;
    inbox->sink_arg  = sink_arg;

    inbox->mb = thread_mailbox_create(WORKER_INBOX_CAPACITY, WORKER_INBOX_BATCH,
                                      worker_inbox_drain, inbox);

    if (inbox->mb == NULL) {
        pefree(inbox, 0);
        return NULL;
    }

    return inbox;
}

bool worker_inbox_post(worker_inbox_t *inbox, http_request_t *req)
{
    if (UNEXPECTED(inbox == NULL || req == NULL)) {
        return false;
    }

    return thread_mailbox_post(inbox->mb, req);
}

size_t worker_inbox_depth(const worker_inbox_t *inbox)
{
    return inbox != NULL ? thread_mailbox_count(inbox->mb) : 0;
}

void worker_inbox_free(worker_inbox_t *inbox)
{
    if (inbox == NULL) {
        return;
    }

    thread_mailbox_free(inbox->mb);
    pefree(inbox, 0);
}
