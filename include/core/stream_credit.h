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

/*
 * Per-stream flow-control credit for the reactor/worker streaming reverse
 * path (#80, step 3). Pure malloc-domain, atomics only — the ONLY object the
 * two threads touch concurrently, so neither side ever dereferences the
 * other's Zend state.
 *
 * Protocol: the worker creates the block (two refs), attaches it to the
 * STREAM_HEADERS wire, and thereafter tracks posted-bytes locally. When
 * posted - acked exceeds its cap, the producer coroutine sleeps on a short
 * timer and re-reads `acked` — no cross-thread event objects. The reactor
 * (owner of the second ref) advances `acked` as the QUIC peer acknowledges
 * stream bytes, and sets `dead` when the stream dies (RST / connection
 * close / failed submit) so a waiting producer unblocks into the standard
 * stream-dead path. Each side releases its ref exactly once; the last
 * release frees the block.
 */

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

/* Reactor thread ONLY (single writer) — load+store instead of fetch-add is
 * safe because nothing else ever writes `acked`. */
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

/* Drop-site helper: nobody will retire this side's bytes anymore — unblock
 * a parked producer FIRST (mark dead), then release this side's ref.
 * NULL-safe. The ordering is the correctness story: a producer must be able
 * to observe `dead` before the block can vanish. Use this at every point a
 * credit-carrying wire dies undelivered or a stream is torn down. */
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
