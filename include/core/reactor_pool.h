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
 * worker that runs a libuv reactor loop (`ZEND_ASYNC_REACTOR_EXECUTE`) and
 * NEVER executes a PHP handler. PHP business logic lives on a separate worker
 * tier; the two are bridged by the #81 lock-free mailbox
 * (`include/core/thread_mailbox.h`). This header is only the substrate: it
 * stands the reactor threads up and tears them down. Listener ownership,
 * request marshalling (D2), response buffers (D3), and dispatch (D5) build on
 * top of it later.
 *
 * Why ThreadPool and not raw pthreads: a reactor must speak ZEND_ASYNC (its
 * loop is thread-local `ASYNC_G(uvloop)`, and the #81 mailbox registers a
 * trigger event on it), which only exists on a Zend thread. A ThreadPool
 * worker is exactly that — see docs/PLAN_REACTOR_POOL.md D1.
 *
 * Threading contract:
 *   - reactor_pool_create()/destroy() run on the owning (parent) thread.
 *   - Each reactor loop runs on its own pool thread; PHP never runs there.
 *   - Stop is cooperative and race-free: destroy() sets a per-reactor atomic
 *     flag; each reactor observes it on its own periodic wakeup and exits its
 *     loop. The parent never touches a reactor's libuv handles cross-thread.
 */

typedef struct reactor_pool_s reactor_pool_t;

/* Create a pool of `reactors` transport reactor threads. `tick_ms` is the
 * reactor's self-wakeup period — it bounds shutdown latency and, in this
 * substrate stage, drives the observable liveness tick. Returns NULL on bad
 * arguments, if the ThreadPool API is unavailable, or on allocation/spawn
 * failure (a PHP exception may be set). Call on the parent thread. */
reactor_pool_t *reactor_pool_create(int reactors, unsigned tick_ms);

/* Number of reactor threads in the pool. */
int reactor_pool_count(const reactor_pool_t *rp);

/* Liveness tick count for reactor `idx` (0-based). Monotonic; advances once
 * per reactor loop wakeup. Substrate-stage observability: a non-zero, rising
 * value proves the loop is running on a PHP-free thread. Returns 0 for an
 * out-of-range index. */
uint64_t reactor_pool_ticks(const reactor_pool_t *rp, int idx);

/* Signal every reactor to stop, wait for the loops to exit, and release the
 * pool. Parent-thread only; safe to call exactly once. */
void reactor_pool_destroy(reactor_pool_t *rp);

#endif /* REACTOR_POOL_H */
