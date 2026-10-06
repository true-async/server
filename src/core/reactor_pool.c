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
#include "core/atomic_pointer.h"
#include "fiu-local.h"
#include "core/reactor_cmd.h"
#include "core/thread_mailbox.h"

#ifdef PHP_WIN32
# include <windows.h>
#else
# include <time.h>
#endif

/* Inbound mailbox sizing. Bounded so a stalled reactor backpressures producers
 * rather than growing unbounded. REACTOR_MAILBOX_CAPACITY is the default depth;
 * HttpServerConfig::setReactorMailboxCapacity() overrides it per pool. */
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
    http_atomic_pointer_t controls;
    zend_async_trigger_event_t *control_wake;
    reactor_capacity_gate_t *capacity_gates;
    bool                  stopping;
} reactor_ctx_t;
static void reactor_epilogue(void);

static void reactor_control_drain(reactor_ctx_t *rc)
{
    reactor_control_t *head = http_atomic_pointer_exchange(&rc->controls, NULL);
    reactor_control_t *ordered = NULL;
    while (head != NULL) {
        reactor_control_t *next = head->next;
        head->next = ordered;
        ordered = head;
        head = next;
    }
    while (ordered != NULL) {
        reactor_control_t *node = ordered;
        ordered = node->next;
        node->fn(node->arg); /* callback may free the node */
    }
}

static void reactor_control_signal(zend_async_event_t *event,
                                   zend_async_event_callback_t *callback,
                                   void *result, zend_object *exception)
{
    (void)callback; (void)result; (void)exception;
    reactor_ctx_t *rc = *(reactor_ctx_t **)((char *)event + event->extra_offset);
    reactor_control_drain(rc);
    reactor_epilogue(); /* resets must flush even when no data post follows */
}

struct reactor_pool_s {
    zend_async_thread_pool_t *tp;
    reactor_ctx_t            *ctx;              /* [count] */
    int                       count;
    size_t                    mailbox_capacity; /* per-reactor inbound depth */
};

/* See reactor_pool_set_drain_epilogue. Process-wide, set once on the parent
 * before reactors run, read on each reactor thread at drain-batch end. */
static void (*g_drain_epilogue)(void) = NULL;

static void reactor_epilogue(void)
{
    if (g_drain_epilogue != NULL) g_drain_epilogue();
}

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

    /* dequeue already freed the ring slots; every subscribed worker may retry.
     * Wakes are bounded by the number of registered worker/ reactor pairs. */
    for (reactor_capacity_gate_t *g = rc->capacity_gates; g != NULL; g = g->next) {
        g->wake(g->arg);
    }

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

    thread_cmd_mailbox_t *const mb = thread_cmd_mailbox_create(rc->pool->mailbox_capacity,
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
    rc->control_wake = ZEND_ASYNC_NEW_TRIGGER_EVENT_EX(sizeof(reactor_ctx_t *));
    if (rc->control_wake == NULL) {
        thread_cmd_mailbox_free(mb);
        rc->mailbox = NULL;
        zend_atomic_int_store_ex(&rc->phase, REACTOR_PHASE_DONE);
        return;
    }
    *(reactor_ctx_t **)((char *)&rc->control_wake->base
                        + rc->control_wake->base.extra_offset) = rc;
    zend_async_event_callback_t *control_cb =
        ZEND_ASYNC_EVENT_CALLBACK(reactor_control_signal);
    if (control_cb == NULL
        || !rc->control_wake->base.add_callback(&rc->control_wake->base, control_cb)) {
        if (control_cb != NULL) {
            ZEND_ASYNC_EVENT_CALLBACK_RELEASE(control_cb);
        }
        rc->control_wake->base.dispose(&rc->control_wake->base);
        rc->control_wake = NULL;
        thread_cmd_mailbox_free(mb);
        rc->mailbox = NULL;
        zend_atomic_int_store_ex(&rc->phase, REACTOR_PHASE_DONE);
        return;
    }
    zend_atomic_int_store_ex(&rc->phase, REACTOR_PHASE_RUN); /* release        */

    while (!rc->stopping) {
        ZEND_ASYNC_REACTOR_EXECUTE(/*no_wait=*/false);
    }

    thread_cmd_mailbox_keepalive(mb, false);
    reactor_control_drain(rc);
    rc->control_wake->base.dispose(&rc->control_wake->base);
    rc->control_wake = NULL;
    thread_cmd_mailbox_free(mb);                            /* consumer-thread */
    rc->mailbox = NULL;

    zend_atomic_int_store_ex(&rc->phase, REACTOR_PHASE_DONE);
}

reactor_pool_t *reactor_pool_create(const int reactors, const size_t mailbox_capacity)
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
    /* 0 = default; never below the batch, or try_enqueue can't accept and the
     * reverse path wedges. */
    rp->mailbox_capacity = mailbox_capacity > 0 ? mailbox_capacity : REACTOR_MAILBOX_CAPACITY;
    if (rp->mailbox_capacity < REACTOR_MAILBOX_BATCH) {
        rp->mailbox_capacity = REACTOR_MAILBOX_BATCH;
    }

    for (int i = 0; i < reactors; i++) {
        rp->ctx[i].pool     = rp;
        rp->ctx[i].mailbox  = NULL;
        rp->ctx[i].stopping = false;
        ZEND_ATOMIC_INT_INIT(&rp->ctx[i].phase, REACTOR_PHASE_SPAWN);
        ZEND_ATOMIC_INT64_INIT(&rp->ctx[i].processed, 0);
        ZEND_ATOMIC_INT64_INIT(&rp->ctx[i].controls, 0);

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

    /* `done` can't ride the ring by value — the reactor would ack the copy.
     * Stack atomic + pointer; we block below, so the frame outlives the store. */
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
        if (zend_atomic_int_load_ex(&rc->phase) == REACTOR_PHASE_DONE) {
            /* Loop exited: mailbox_free discards still-queued cmds (they can
             * never run — no stack UAF), it runs before the DONE store. One
             * final check catches a cmd that executed in the last drain. */
            return zend_atomic_int_load_ex(&done) != 0;
        }

        reactor_pool_msleep();
    }

    return true;
}

bool reactor_pool_post_exec(reactor_pool_t *rp, int idx, reactor_exec_fn fn, void *arg)
{
    return reactor_pool_try_post_exec(rp, idx, fn, arg) == THREAD_QUEUE_ACCEPTED;
}
thread_queue_result_t reactor_pool_try_post_exec(reactor_pool_t *rp, int idx,
                                                reactor_exec_fn fn, void *arg)
{
    if (!reactor_pool_is_running(rp, idx) || fn == NULL) return THREAD_QUEUE_STOPPED;
    /* Fault seam: force FULL and model a consumer drain with a NOOP. The
     * notifier still runs on the reactor, exercising subscribe/recheck/wake. */
    fiu_do_on("h3/reverse/queue_full", {
        (void)reactor_pool_post(rp, idx, NULL);
        return THREAD_QUEUE_FULL;
    });
    reactor_ctx_t *rc = &rp->ctx[idx];
    reactor_cmd_t cmd = {0};
    cmd.kind = REACTOR_CMD_POST; cmd.fn = fn; cmd.arg = arg;
    return thread_cmd_mailbox_try_post(rc->mailbox, &cmd);
}

bool reactor_pool_is_running(const reactor_pool_t *rp, const int idx)
{
    if (UNEXPECTED(rp == NULL || idx < 0 || idx >= rp->count)) {
        return false;
    }

    return zend_atomic_int_load_ex(&rp->ctx[idx].phase) == REACTOR_PHASE_RUN;
}

bool reactor_pool_has_capacity(const reactor_pool_t *rp, int idx)
{
    return reactor_pool_is_running(rp, idx)
        && thread_cmd_mailbox_count(rp->ctx[idx].mailbox) < rp->mailbox_capacity;
}

bool reactor_pool_post_control(reactor_pool_t *rp, const int idx,
                                reactor_control_t *node)
{
    if (!reactor_pool_is_running(rp, idx) || node == NULL || node->fn == NULL) {
        return false;
    }
    reactor_ctx_t *rc = &rp->ctx[idx];
    void *head = http_atomic_pointer_load(&rc->controls);
    do {
        node->next = head;
    } while (!http_atomic_pointer_compare_exchange(&rc->controls, &head, node));
    rc->control_wake->trigger(rc->control_wake);
    return true;
}

typedef struct {
    reactor_ctx_t *rc;
    reactor_capacity_gate_t *gate;
} capacity_registration_t;

typedef struct {
    reactor_control_t node;
    reactor_exec_fn fn;
    void *arg;
    zend_atomic_int done;
} control_fence_t;

static void control_fence_apply(void *arg)
{
    control_fence_t *f = arg;
    f->fn(f->arg);
    zend_atomic_int_store_ex(&f->done, 1);
}

bool reactor_pool_control_exec(reactor_pool_t *rp, int idx,
                               reactor_exec_fn fn, void *arg)
{
    control_fence_t f = {0};
    f.fn = fn; f.arg = arg;
    f.node.fn = control_fence_apply; f.node.arg = &f;
    ZEND_ATOMIC_INT_INIT(&f.done, 0);
    if (!reactor_pool_post_control(rp, idx, &f.node)) return false;
    /* A posted stack node cannot be abandoned on cancellation. This cold
     * fence waits for its acknowledgement, not for data-mailbox capacity. */
    while (zend_atomic_int_load_ex(&f.done) == 0) reactor_pool_msleep();
    return true;
}

static void capacity_register(void *arg)
{
    capacity_registration_t *r = arg;
    r->gate->next = r->rc->capacity_gates;
    r->rc->capacity_gates = r->gate;
}

static void capacity_unregister(void *arg)
{
    capacity_registration_t *r = arg;
    reactor_capacity_gate_t **p = &r->rc->capacity_gates;
    while (*p != NULL && *p != r->gate) p = &(*p)->next;
    if (*p != NULL) *p = r->gate->next;
}

bool reactor_pool_capacity_register(reactor_pool_t *rp, int idx,
                                    reactor_capacity_gate_t *gate)
{
    if (!reactor_pool_is_running(rp, idx) || gate == NULL) return false;
    capacity_registration_t r = { &rp->ctx[idx], gate };
    return reactor_pool_control_exec(rp, idx, capacity_register, &r);
}

bool reactor_pool_capacity_unregister(reactor_pool_t *rp, int idx,
                                      reactor_capacity_gate_t *gate)
{
    if (!reactor_pool_is_running(rp, idx)) return true;
    capacity_registration_t r = { &rp->ctx[idx], gate };
    return reactor_pool_control_exec(rp, idx, capacity_unregister, &r);
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
