/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

#ifndef REACTOR_POOL_H
#define REACTOR_POOL_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

/*
 * Reactor thread pool (issue #80, design D1 — substrate).
 *
 * A pool of pure-C transport reactor threads: each is a TrueAsync ThreadPool
 * worker that runs its libuv reactor loop and NEVER executes a PHP handler. PHP
 * business logic lives on a separate worker tier; the two are bridged by the
 * #81 lock-free mailbox (include/core/thread_mailbox.h).
 *
 * A reactor does not "tick": it runs the native event loop and is woken by real
 * events. At this substrate stage its only event source is its own inbound
 * mailbox; once the transport lands (D2) the same loop is woken by the listener
 * socket and QUIC timers too. Liveness is therefore not a heartbeat — it is
 * whether the inbound channel is being drained (reactor_pool_processed). A
 * consumer that stops draining is exactly a stalled reactor; a bounded mailbox
 * turns that into backpressure at the producer (reactor_pool_post returning
 * false), which is the real health signal. See docs/PLAN_REACTOR_POOL.md.
 *
 * Threading contract:
 *   - reactor_pool_create()/destroy() run on the owning (parent) thread.
 *   - Each reactor loop runs on its own pool thread; PHP never runs there.
 *   - Shutdown is cooperative and travels through the channel: destroy() posts
 *     a stop sentinel into each reactor's mailbox; the reactor observes it on
 *     its normal drain and leaves its loop. The parent never touches a
 *     reactor's libuv handles cross-thread.
 *   - Producers (reactor_pool_post) must quiesce before destroy().
 */

typedef struct reactor_pool_s reactor_pool_t;

/* Stand up `reactors` transport reactor threads and block until each has
 * entered its loop (or failed to). Returns NULL on bad arguments, if the
 * ThreadPool API is unavailable, or if no reactor came up (a PHP exception may
 * be set). Call on the parent thread. */
reactor_pool_t *reactor_pool_create(int reactors);

/* Number of reactor threads that came up. */
int reactor_pool_count(const reactor_pool_t *rp);

/* Post an opaque item into reactor `idx`'s inbound mailbox. Returns false if
 * idx is out of range, the reactor is not running, or the bounded mailbox is
 * full (backpressure — the caller decides to drop/retry). Any thread; must not
 * race destroy(). */
bool reactor_pool_post(reactor_pool_t *rp, int idx, void *item);

/* Count of items reactor `idx` has drained from its inbound. Rises as the
 * reactor services work — "alive" == "draining". Returns 0 for a bad index. */
uint64_t reactor_pool_processed(const reactor_pool_t *rp, int idx);

/* Signal every reactor to stop (via the channel), wait for the loops to leave,
 * and release the pool. Parent-thread only; call once, after producers quiesce.
 * Passing NULL is a no-op. */
void reactor_pool_destroy(reactor_pool_t *rp);

#endif /* REACTOR_POOL_H */
