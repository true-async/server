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

/* Per-stream flow-control credit shared worker↔reactor. Malloc + atomics
 * only — the one object both threads touch. Worker creates it with two refs
 * (one per side); reactor advances `acked` on peer ACK and sets `dead` on
 * stream death; last release frees. */

typedef struct {
    zend_atomic_int64 acked;   /* bytes the reactor has retired (peer ACK) */
    zend_atomic_int   dead;    /* stream died — producer must stop waiting */
    zend_atomic_int   refs;
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

/* NULL-safe. Dead must be set before release so a parked producer sees it. */
static inline void stream_credit_abandon(stream_credit_t *sc)
{
    if (sc != NULL) {
        stream_credit_mark_dead(sc);
        stream_credit_release(sc);
    }
}

static inline bool stream_credit_is_dead(const stream_credit_t *sc)
{
    return zend_atomic_int_load_ex(&((stream_credit_t *)sc)->dead) != 0;
}

#endif /* STREAM_CREDIT_H */
