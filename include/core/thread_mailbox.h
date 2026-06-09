/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

#ifndef THREAD_MAILBOX_H
#define THREAD_MAILBOX_H

#include <stddef.h>
#include <stdbool.h>

/*
 * Reactor-integrated MPSC mailbox (issue #81): a bounded thread_mpsc_t wired to
 * the consumer's event loop via a zend_async_trigger_event_t (uv_async). Any
 * thread posts; the owning worker's reactor is woken once per empty->non-empty
 * edge and drains the queue in batches on its own thread.
 *
 * This is the non-blocking handoff primitive the cross-worker HTTP/3 path (#72)
 * and the WebSocket outbound/offload paths (#2) build on: the producer never
 * blocks (full => clean backpressure), and the consumer never touches the queue
 * off its reactor thread.
 *
 * Lost-wakeup safety: the producer signals only on the empty->non-empty edge,
 * the enqueue (release) happens-before that signal, and the consumer drains to
 * empty before returning. uv_async coalescing plus drain-to-empty means no item
 * is ever stranded.
 *
 * Threading contract:
 *   - thread_mailbox_create()/free() run on the consumer's reactor thread (they
 *     create/dispose a libuv handle on that loop).
 *   - thread_mailbox_post() runs on any thread.
 *   - free() must run after producers have quiesced.
 */

typedef struct thread_mailbox_s thread_mailbox_t;

/* Drain callback, invoked on the consumer's reactor thread with a batch of
 * 1..batch items. Ownership of each item passes to the callback. */
typedef void (*thread_mailbox_drain_fn)(void **items, size_t count, void *arg);

/* Create a mailbox holding at most `capacity` items, draining up to `batch`
 * per pass. Returns NULL on bad arguments, allocation failure, or if no reactor
 * is running on the calling thread. */
thread_mailbox_t *thread_mailbox_create(size_t capacity, size_t batch,
                                        thread_mailbox_drain_fn on_drain, void *arg);

/* Dispose the mailbox. Consumer-thread only; producers must have stopped. */
void thread_mailbox_free(thread_mailbox_t *mb);

/* Producer side — any thread. Returns true if accepted, false if the mailbox is
 * full (the caller decides whether to drop, retry, or close). */
bool thread_mailbox_post(thread_mailbox_t *mb, void *item);

/* Opt-in: make the wakeup handle keep the consumer's reactor loop alive (uv_ref
 * via the trigger's start()). Mailboxes default to NOT keeping the loop alive —
 * they are a wake source for a loop already kept running by other handles (a
 * listener, coroutines). A dedicated reactor thread whose only handle is its
 * inbound mailbox enables this so its loop blocks on the kernel instead of
 * spinning. Consumer-thread only. */
void thread_mailbox_keepalive(thread_mailbox_t *mb, bool enable);

/* Approximate number of queued items. */
size_t thread_mailbox_count(const thread_mailbox_t *mb);

#endif /* THREAD_MAILBOX_H */
