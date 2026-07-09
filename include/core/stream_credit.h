/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

#ifndef STREAM_CREDIT_H
#define STREAM_CREDIT_H

#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>

#include "Zend/zend_atomic.h"
#include "Zend/zend_async_API.h"   /* zend_async_trigger_event_t */

/* Per-stream flow-control credit shared worker↔reactor. Malloc + atomics
 * only — the one object both threads touch. Worker creates it with two refs
 * (one per side); reactor advances `acked` on peer ACK and sets `dead` on
 * stream death; last release frees. */

typedef struct {
    zend_atomic_int64 acked;   /* bytes the reactor has retired (peer ACK) */
    zend_atomic_int   dead;    /* stream died — producer must stop waiting */
    zend_atomic_int   refs;
    zend_atomic_ptr   waker;      /* worker-owned trigger the reactor signals */
    zend_atomic_int   waker_busy; /* count of signallers inside trigger() (0 = idle) */
} stream_credit_t;

static inline stream_credit_t *stream_credit_create(void)
{
    stream_credit_t *const sc = (stream_credit_t *)calloc(1, sizeof(*sc));

    if (sc == NULL) {
        return NULL;
    }

    ZEND_ATOMIC_INT64_INIT(&sc->acked, 0);
    ZEND_ATOMIC_INT_INIT(&sc->dead, 0);
    ZEND_ATOMIC_INT_INIT(&sc->refs, 2);   /* worker ctx + reactor stream */
    ZEND_ATOMIC_PTR_INIT(&sc->waker, NULL);
    ZEND_ATOMIC_INT_INIT(&sc->waker_busy, 0);

    return sc;
}

static inline void stream_credit_release(stream_credit_t *sc)
{
    if (sc != NULL && zend_atomic_int_fetch_sub(&sc->refs, 1) == 1) {
        free(sc);
    }
}

/* Reactor thread only — single writer, so load+store beats fetch-add. */
static inline void stream_credit_ack(stream_credit_t *sc, const uint64_t bytes)
{
    zend_atomic_int64_store_ex(&sc->acked,
        zend_atomic_int64_load_ex(&sc->acked) + (int64_t)bytes);
}

static inline uint64_t stream_credit_acked(const stream_credit_t *sc)
{
    return (uint64_t)zend_atomic_int64_load_ex(
        &((stream_credit_t *)sc)->acked);
}

static inline void stream_credit_mark_dead(stream_credit_t *sc)
{
    zend_atomic_int_store_ex(&sc->dead, 1);
}

/* busy brackets load+trigger so clear_waker can't dispose the event mid-signal;
 * seq-cst makes it a StoreLoad fence. Counter (not flag) survives overlap. */
static inline void stream_credit_wake(stream_credit_t *sc)
{
    zend_atomic_int_inc(&sc->waker_busy);

    zend_async_trigger_event_t *const t =
        (zend_async_trigger_event_t *)zend_atomic_ptr_load_ex(&sc->waker);

    if (t != NULL) {
        t->trigger(t);
    }

    zend_atomic_int_dec(&sc->waker_busy);
}

/* Worker thread: publish/retract the park trigger. clear must complete
 * before the worker disposes the event. */
static inline void stream_credit_set_waker(stream_credit_t *sc,
                                           zend_async_trigger_event_t *t)
{
    zend_atomic_ptr_store_ex(&sc->waker, t);
}

/* Spin-wait hint: keep the core polite (SMT sibling + power) while the
 * reactor finishes an in-flight trigger(). The wait is a few instructions
 * long; a descheduled reactor is the only way it stretches. */
static inline void stream_credit_spin_pause(void)
{
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    __asm__ __volatile__("yield");
#endif
}

static inline void stream_credit_clear_waker(stream_credit_t *sc)
{
    zend_atomic_ptr_store_ex(&sc->waker, NULL);

    /* wait out a signal that loaded the pointer just before the NULL store */
    while (zend_atomic_int_load_ex(&sc->waker_busy) != 0) {
        stream_credit_spin_pause();
    }
}

/* NULL-safe. Dead must be set before release so a parked producer sees it;
 * wake so it sees it NOW instead of at its next timeout. */
static inline void stream_credit_abandon(stream_credit_t *sc)
{
    if (sc != NULL) {
        stream_credit_mark_dead(sc);
        stream_credit_wake(sc);
        stream_credit_release(sc);
    }
}

static inline bool stream_credit_is_dead(const stream_credit_t *sc)
{
    return zend_atomic_int_load_ex(&((stream_credit_t *)sc)->dead) != 0;
}

#endif /* STREAM_CREDIT_H */
