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
#include "core/reactor_cmd.h"
#include "core/thread_mailbox.h"

#ifdef PHP_WIN32
# include <windows.h>
#else
# include <time.h>
#endif

/* Inbound mailbox sizing. Bounded so a stalled reactor backpressures producers
 * rather than growing unbounded. */
#define REACTOR_MAILBOX_CAPACITY 1024
#define REACTOR_MAILBOX_BATCH      64

/* ctx lifecycle, published from the reactor thread to the parent. */
#define REACTOR_PHASE_SPAWN 0   /* submitted, not yet in its loop          */
#define REACTOR_PHASE_RUN   1   /* mailbox created and published; looping  */
#define REACTOR_PHASE_DONE  2   /* loop left, mailbox freed                */

/* Per-reactor state, shared parent <-> one reactor thread — the legitimate
 * cross-thread handshake (Zend atomics), not single-threaded-core state.
 * `mailbox` is written by the reactor before it stores phase=RUN, and read by
 * the parent only after it observes phase>=RUN — the atomic phase store/load
 * orders the plain write. `stopping` is touched only on the reactor thread
 * (loop + drain callback). */
typedef struct {
    reactor_pool_t       *pool;
    zend_atomic_int       phase;
    thread_cmd_mailbox_t *mailbox;
    zend_atomic_int64     processed;
    bool                  stopping;
} reactor_ctx_t;

struct reactor_pool_s {
    zend_async_thread_pool_t *tp;
    reactor_ctx_t            *ctx;     /* [count] */
    int                       count;
};

/* See reactor_pool_set_drain_epilogue. Process-wide, set once on the parent
 * before reactors run, read on each reactor thread at drain-batch end. */
static void (*g_drain_epilogue)(void) = NULL;

void reactor_pool_set_drain_epilogue(void (*fn)(void))
{
    g_drain_epilogue = fn;
}

static void reactor_pool_msleep(void)
{
#ifdef PHP_WIN32
    Sleep(1);
#else
    const struct timespec ts = { 0, 1000000 }; /* 1 ms */
    nanosleep(&ts, NULL);
#endif
}

/* Runs on the reactor thread when its inbound mailbox has items. Commands
 * arrive by value; STOP asks the loop to leave, everything else is real work,
 * counted here. Nothing is freed — the ring owned the storage, not the heap. */
static void reactor_drain(reactor_cmd_t *items, const size_t count, void *arg)
{
    reactor_ctx_t *const rc = (reactor_ctx_t *)arg;
    int64_t              drained = 0;

    for (size_t i = 0; i < count; i++) {
        reactor_cmd_t *const cmd = &items[i];

        switch (cmd->kind) {
            case REACTOR_CMD_STOP:
                rc->stopping = true;
                continue;

            case REACTOR_CMD_EXEC:
                cmd->fn(cmd->arg);
                /* Release: publish fn's effects before the caller sees done.
                 * done points at the blocking caller's stack atomic. */
                zend_atomic_int_store_ex((zend_atomic_int *)cmd->done, 1);
                break;

            case REACTOR_CMD_POST:
                cmd->fn(cmd->arg);
                break;

            case REACTOR_CMD_NOOP:
            default:
                break;
        }

        drained++;
    }

    /* Batch epilogue: coalesce any per-command deferred work (H3 steer flush)
     * into one pass now that every command in this drain has run. */
    if (g_drain_epilogue != NULL) {
        g_drain_epilogue();
    }

    if (drained != 0) {
        zend_atomic_int64_store_ex(&rc->processed,
                                   zend_atomic_int64_load_ex(&rc->processed) + drained);
    }
}

/* The reactor loop. Owns a pure-C libuv loop, kept alive by its inbound mailbox
 * (the trigger is ref'd via keepalive); blocks in the kernel until woken, then
 * drains. Leaves when a stop sentinel arrives. No PHP executes here. */
static void reactor_loop_handler(zend_async_event_t *event, void *vctx)
{
    (void)event;
    reactor_ctx_t *const rc = (reactor_ctx_t *)vctx;

    thread_cmd_mailbox_t *const mb = thread_cmd_mailbox_create(REACTOR_MAILBOX_CAPACITY,
                                                              REACTOR_MAILBOX_BATCH,
                                                              reactor_drain, rc);

    if (mb == NULL) {
        zend_atomic_int_store_ex(&rc->phase, REACTOR_PHASE_DONE);
        return;
    }

    /* Open inbound keeps the loop alive (no listener yet) — so uv_run blocks
     * instead of spinning. */
    thread_cmd_mailbox_keepalive(mb, true);

    rc->mailbox = mb;                                       /* publish (plain) */
    zend_atomic_int_store_ex(&rc->phase, REACTOR_PHASE_RUN); /* release        */

    while (!rc->stopping) {
        ZEND_ASYNC_REACTOR_EXECUTE(/*no_wait=*/false);
    }

    thread_cmd_mailbox_keepalive(mb, false);
    thread_cmd_mailbox_free(mb);                            /* consumer-thread */
    rc->mailbox = NULL;

    zend_atomic_int_store_ex(&rc->phase, REACTOR_PHASE_DONE);
}

reactor_pool_t *reactor_pool_create(const int reactors)
{
    if (reactors <= 0) {
        return NULL;
    }

    if (zend_async_new_thread_pool_fn == NULL) {
        zend_throw_error(NULL, "ThreadPool API is not registered — load true_async first");
        return NULL;
    }

    zend_async_thread_pool_t *const tp =
        ZEND_ASYNC_NEW_THREAD_POOL((int32_t)reactors, (int32_t)reactors);

    if (tp == NULL || tp->submit_internal == NULL) {
        if (tp != NULL) {
            ZEND_THREAD_POOL_DELREF(tp);
        }

        zend_throw_error(NULL, "ThreadPool->submit_internal not available — true_async too old");
        return NULL;
    }

    reactor_pool_t *const rp = pecalloc(1, sizeof(*rp), 0);
    rp->tp    = tp;
    rp->ctx   = pecalloc((size_t)reactors, sizeof(reactor_ctx_t), 0);
    rp->count = 0;

    for (int i = 0; i < reactors; i++) {
        rp->ctx[i].pool     = rp;
        rp->ctx[i].mailbox  = NULL;
        rp->ctx[i].stopping = false;
        ZEND_ATOMIC_INT_INIT(&rp->ctx[i].phase, REACTOR_PHASE_SPAWN);
        ZEND_ATOMIC_INT64_INIT(&rp->ctx[i].processed, 0);

        zend_async_event_t *const evt =
            tp->submit_internal(tp, reactor_loop_handler, &rp->ctx[i]);

        if (evt == NULL) {
            break;
        }

        rp->count++;

        /* We track completion via the per-reactor phase, not this future —
         * release our reference so the unawaited future does not leak. */
        ZEND_ASYNC_EVENT_RELEASE(evt);
    }

    if (rp->count == 0) {
        ZEND_THREAD_POOL_DELREF(tp);
        pefree(rp->ctx, 0);
        pefree(rp, 0);
        return NULL;
    }

    /* Block until every submitted reactor has reached its loop (or failed) so
     * the pool is ready to accept posts the moment we return. */
    for (int i = 0; i < rp->count; i++) {
        while (zend_atomic_int_load_ex(&rp->ctx[i].phase) == REACTOR_PHASE_SPAWN) {
            reactor_pool_msleep();
        }
    }

    return rp;
}

int reactor_pool_count(const reactor_pool_t *rp)
{
    return rp != NULL ? rp->count : 0;
}

bool reactor_pool_post(reactor_pool_t *rp, const int idx, void *item)
{
    if (UNEXPECTED(rp == NULL || idx < 0 || idx >= rp->count)) {
        return false;
    }

    reactor_ctx_t *const rc = &rp->ctx[idx];

    if (UNEXPECTED(zend_atomic_int_load_ex(&rc->phase) != REACTOR_PHASE_RUN)) {
        return false;
    }

    reactor_cmd_t cmd = {0};
    cmd.kind    = REACTOR_CMD_NOOP;
    cmd.payload = item;

    return thread_cmd_mailbox_post(rc->mailbox, &cmd);
}

bool reactor_pool_exec(reactor_pool_t *rp, const int idx, const reactor_exec_fn fn, void *arg)
{
    if (UNEXPECTED(rp == NULL || idx < 0 || idx >= rp->count || fn == NULL)) {
        return false;
    }

    reactor_ctx_t *const rc = &rp->ctx[idx];

    if (UNEXPECTED(zend_atomic_int_load_ex(&rc->phase) != REACTOR_PHASE_RUN)) {
        return false;
    }

    /* The envelope is copied into the ring by value, so `done` cannot be an
     * embedded atomic — the reactor would ack the ring's copy. Keep the atomic
     * on our stack and hand the reactor a pointer to it; we block on it below,
     * so the frame outlives the reactor's store. */
    zend_atomic_int done;
    ZEND_ATOMIC_INT_INIT(&done, 0);

    reactor_cmd_t cmd = {0};
    cmd.kind = REACTOR_CMD_EXEC;
    cmd.fn   = fn;
    cmd.arg  = arg;
    cmd.done = &done;

    /* Bounded mailbox: retry on full; bail if the reactor leaves RUN. */
    while (!thread_cmd_mailbox_post(rc->mailbox, &cmd)) {
        if (zend_atomic_int_load_ex(&rc->phase) != REACTOR_PHASE_RUN) {
            return false;
        }

        reactor_pool_msleep();
    }

    /* Acquire: pair with the reactor's release store once fn has run. */
    while (zend_atomic_int_load_ex(&done) == 0) {
        reactor_pool_msleep();
    }

    return true;
}

bool reactor_pool_post_exec(reactor_pool_t *rp, const int idx,
                            const reactor_exec_fn fn, void *arg)
{
    if (UNEXPECTED(rp == NULL || idx < 0 || idx >= rp->count || fn == NULL)) {
        return false;
    }

    reactor_ctx_t *const rc = &rp->ctx[idx];

    if (UNEXPECTED(zend_atomic_int_load_ex(&rc->phase) != REACTOR_PHASE_RUN)) {
        return false;
    }

    /* Fire-and-forget: the value rides the ring, no `done` ack, nothing to free. */
    reactor_cmd_t cmd = {0};
    cmd.kind = REACTOR_CMD_POST;
    cmd.fn   = fn;
    cmd.arg  = arg;

    return thread_cmd_mailbox_post(rc->mailbox, &cmd);
}

uint64_t reactor_pool_processed(const reactor_pool_t *rp, const int idx)
{
    if (UNEXPECTED(rp == NULL || idx < 0 || idx >= rp->count)) {
        return 0;
    }

    return (uint64_t)zend_atomic_int64_load_ex(&rp->ctx[idx].processed);
}

void reactor_pool_destroy(reactor_pool_t *rp)
{
    if (rp == NULL) {
        return;
    }

    /* Ask each running reactor to leave by posting a STOP command into its
     * mailbox — same path as real work, so no cross-thread handle touch. */
    reactor_cmd_t stop = {0};
    stop.kind = REACTOR_CMD_STOP;

    for (int i = 0; i < rp->count; i++) {
        reactor_ctx_t *const rc = &rp->ctx[i];

        if (zend_atomic_int_load_ex(&rc->phase) != REACTOR_PHASE_RUN) {
            continue;
        }

        while (!thread_cmd_mailbox_post(rc->mailbox, &stop)) {
            reactor_pool_msleep();
        }
    }

    /* Wait for every loop to leave — only then is the ctx no longer touched. */
    for (int i = 0; i < rp->count; i++) {
        while (zend_atomic_int_load_ex(&rp->ctx[i].phase) != REACTOR_PHASE_DONE) {
            reactor_pool_msleep();
        }
    }

    rp->tp->close(rp->tp);
    ZEND_THREAD_POOL_DELREF(rp->tp);

    pefree(rp->ctx, 0);
    pefree(rp, 0);
}
