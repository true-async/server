/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+

  Reactor thread pool (#80, design D1 — substrate). See include/core/reactor_pool.h.
*/

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include "php.h"
#include "Zend/zend_async_API.h"
#include "Zend/zend_atomic.h"
#include "core/reactor_pool.h"

#ifdef PHP_WIN32
# include <windows.h>
#else
# include <time.h>
#endif

/* Per-reactor state. Lives in the parent-owned ctx array; the reactor thread
 * reads `stop` and writes `ticks`, the parent reads `ticks` and writes `stop`.
 * `pool` back-points so the loop handler can reach tick_ms and the live count. */
typedef struct {
    reactor_pool_t   *pool;
    zend_atomic_int   stop;    /* parent sets 1 -> loop exits                 */
    zend_atomic_int64 ticks;   /* reactor bumps once per loop wakeup          */
} reactor_ctx_t;

struct reactor_pool_s {
    zend_async_thread_pool_t *tp;       /* underlying ThreadPool               */
    reactor_ctx_t            *ctx;      /* [count]                             */
    int                       count;    /* reactors successfully submitted     */
    unsigned                  tick_ms;  /* reactor self-wakeup period          */
    zend_atomic_int           live;     /* loop handlers submitted-but-running */
};

static void reactor_pool_msleep(void)
{
#ifdef PHP_WIN32
    Sleep(1);
#else
    const struct timespec ts = { 0, 1000000 }; /* 1 ms */
    nanosleep(&ts, NULL);
#endif
}

/* Fires on the reactor's own thread every tick_ms. Pure liveness bump — the
 * wakeup also gives the loop its chance to re-check `stop`. */
static void reactor_tick_cb(zend_async_event_t *event, zend_async_event_callback_t *callback,
                            void *result, zend_object *exception)
{
    (void)callback;
    (void)result;
    (void)exception;

    reactor_ctx_t *rc = *(reactor_ctx_t **)((char *)event + event->extra_offset);
    zend_atomic_int64_store_ex(&rc->ticks, zend_atomic_int64_load_ex(&rc->ticks) + 1);
}

/* Runs on a ThreadPool worker thread for the reactor's whole life. Owns a
 * pure-C libuv reactor loop and never executes PHP. Exits when the parent
 * sets `stop`; the periodic timer guarantees the loop wakes to observe it. */
static void reactor_loop_handler(zend_async_event_t *event, void *vctx)
{
    (void)event;
    reactor_ctx_t  *rc = (reactor_ctx_t *)vctx;
    reactor_pool_t *rp = rc->pool;

    zend_async_timer_event_t *timer = (zend_async_timer_event_t *)
        ZEND_ASYNC_NEW_TIMER_EVENT_EX((zend_ulong)rp->tick_ms, /*is_periodic=*/true,
                                      sizeof(reactor_ctx_t *));

    if (timer == NULL) {
        /* Could not arm; nothing ran, but the parent counted this submit in
         * `live`, so balance it before returning. */
        zend_atomic_int_dec(&rp->live);
        return;
    }

    *(reactor_ctx_t **)((char *)&timer->base + timer->base.extra_offset) = rc;
    timer->base.add_callback(&timer->base, ZEND_ASYNC_EVENT_CALLBACK(reactor_tick_cb));
    timer->base.start(&timer->base);

    while (zend_atomic_int_load_ex(&rc->stop) == 0) {
        ZEND_ASYNC_REACTOR_EXECUTE(/*no_wait=*/false);
    }

    /* Dispose the libuv handle on the thread that created it. */
    ZEND_ASYNC_EVENT_SET_CLOSED(&timer->base);
    timer->base.dispose(&timer->base);

    /* Last touch of `rp`/`rc` — after this the parent may free them. */
    zend_atomic_int_dec(&rp->live);
}

reactor_pool_t *reactor_pool_create(const int reactors, const unsigned tick_ms)
{
    if (reactors <= 0 || tick_ms == 0) {
        return NULL;
    }

    if (zend_async_new_thread_pool_fn == NULL) {
        zend_throw_error(NULL, "ThreadPool API is not registered — load true_async first");
        return NULL;
    }

    zend_async_thread_pool_t *tp =
        ZEND_ASYNC_NEW_THREAD_POOL((int32_t)reactors, (int32_t)reactors);

    if (tp == NULL || tp->submit_internal == NULL) {
        if (tp != NULL) {
            ZEND_THREAD_POOL_DELREF(tp);
        }

        zend_throw_error(NULL, "ThreadPool->submit_internal not available — true_async too old");
        return NULL;
    }

    reactor_pool_t *rp = pecalloc(1, sizeof(*rp), 0);
    rp->tp      = tp;
    rp->ctx     = pecalloc((size_t)reactors, sizeof(reactor_ctx_t), 0);
    rp->count   = 0;
    rp->tick_ms = tick_ms;
    ZEND_ATOMIC_INT_INIT(&rp->live, 0);

    for (int i = 0; i < reactors; i++) {
        rp->ctx[i].pool = rp;
        ZEND_ATOMIC_INT_INIT(&rp->ctx[i].stop, 0);
        ZEND_ATOMIC_INT64_INIT(&rp->ctx[i].ticks, 0);

        /* Count the submit in `live` before dispatch: the handler always
         * decrements exactly once, even if it fails to arm its timer. */
        zend_atomic_int_inc(&rp->live);

        zend_async_event_t *evt =
            tp->submit_internal(tp, reactor_loop_handler, &rp->ctx[i]);

        if (evt == NULL) {
            zend_atomic_int_dec(&rp->live);
            break;
        }

        rp->count++;
    }

    if (rp->count == 0) {
        /* Nothing started — unwind. Any in-flight exception is left for the
         * caller. */
        ZEND_THREAD_POOL_DELREF(tp);
        pefree(rp->ctx, 0);
        pefree(rp, 0);
        return NULL;
    }

    return rp;
}

int reactor_pool_count(const reactor_pool_t *rp)
{
    return rp != NULL ? rp->count : 0;
}

uint64_t reactor_pool_ticks(const reactor_pool_t *rp, const int idx)
{
    if (rp == NULL || idx < 0 || idx >= rp->count) {
        return 0;
    }

    return (uint64_t)zend_atomic_int64_load_ex(&rp->ctx[idx].ticks);
}

void reactor_pool_destroy(reactor_pool_t *rp)
{
    if (rp == NULL) {
        return;
    }

    for (int i = 0; i < rp->count; i++) {
        zend_atomic_int_store_ex(&rp->ctx[i].stop, 1);
    }

    /* Wait for every loop to leave its handler — only then is the ctx array
     * no longer touched by any reactor thread. Bounded by tick_ms. */
    while (zend_atomic_int_load_ex(&rp->live) > 0) {
        reactor_pool_msleep();
    }

    /* Reactor loops are gone; close the channel so the worker threads leave
     * their receive loop, then drop our ref (dispose fires on the last). */
    rp->tp->close(rp->tp);
    ZEND_THREAD_POOL_DELREF(rp->tp);

    pefree(rp->ctx, 0);
    pefree(rp, 0);
}
