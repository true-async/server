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

/* A function run on a reactor's own thread by reactor_pool_exec. */
typedef void (*reactor_exec_fn)(void *arg);

/* Run fn(arg) on reactor `idx`'s own loop thread and block the caller until it
 * returns. The reactor executes it inline on its drain pass — this is how
 * transport that must be bound to the reactor's libuv loop (a uv handle, the
 * H3 listener's UDP socket) gets created on the right thread. Returns false for
 * a bad index, a NULL fn, or a non-running reactor. Any thread; must not race
 * destroy(), and the caller must serialise its own exec calls to one reactor
 * (it blocks to completion, so this is natural). */
bool reactor_pool_exec(reactor_pool_t *rp, int idx, reactor_exec_fn fn, void *arg);

/* Like reactor_pool_exec but fire-and-forget: post fn(arg) into reactor `idx`'s
 * inbound and return immediately, without waiting for it to run. The reactor
 * runs it on its drain pass and frees the internal envelope; there is no
 * completion handshake. This is the worker->reactor reverse path's delivery
 * primitive — the worker posts an apply callback + its message and never
 * blocks. Ownership of whatever `arg` points at is the callback's concern (it
 * runs once on the reactor). Returns false for a bad index, a NULL fn, a
 * non-running reactor, or a full mailbox (backpressure — the caller keeps `arg`
 * and decides to drop/retry). Any thread; must not race destroy(). */
bool reactor_pool_post_exec(reactor_pool_t *rp, int idx, reactor_exec_fn fn, void *arg);

/* Count of items reactor `idx` has drained from its inbound. Rises as the
 * reactor services work — "alive" == "draining". Returns 0 for a bad index. */
uint64_t reactor_pool_processed(const reactor_pool_t *rp, int idx);

/* Optional epilogue run on the reactor thread at the END of every mailbox drain
 * batch, after all commands in the batch have run. Lets a consumer coalesce
 * per-command side effects into one action per drain — the H3 steering path uses
 * it to flush forwarded datagrams once per batch instead of once per datagram,
 * matching the recvmmsg tick's single deferred flush. Process-wide, set once on
 * the parent before reactors start; fn runs on each reactor thread, so it must
 * key its state per-thread. NULL clears it. */
void reactor_pool_set_drain_epilogue(void (*fn)(void));

/* Signal every reactor to stop (via the channel), wait for the loops to leave,
 * and release the pool. Parent-thread only; call once, after producers quiesce.
 * Passing NULL is a no-op. */
void reactor_pool_destroy(reactor_pool_t *rp);

#endif /* REACTOR_POOL_H */
