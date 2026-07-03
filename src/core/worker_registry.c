/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+

  Worker registry (#80, B3) — atomic table of per-worker inboxes.
  See include/core/worker_registry.h.
*/

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include "php.h"
#include "TSRM.h"
#include "Zend/zend_atomic.h"
#include "core/worker_registry.h"

struct worker_registry_s {
    zend_atomic_ptr *slots;     /* [capacity], each a worker_inbox_t* or NULL */
    zend_atomic_int  rr;        /* round-robin cursor */
    MUTEX_T          admin;     /* serialises add/retire (rare); picks stay lock-free */
    int              capacity;
};

worker_registry_t *worker_registry_create(const int capacity)
{
    if (capacity <= 0) {
        return NULL;
    }

    worker_registry_t *const reg = pecalloc(1, sizeof(*reg), 0);
    reg->slots    = pecalloc((size_t)capacity, sizeof(zend_atomic_ptr), 0);
    reg->capacity = capacity;
    ZEND_ATOMIC_INT_INIT(&reg->rr, 0);
    reg->admin = tsrm_mutex_alloc();

    for (int i = 0; i < capacity; i++) {
        ZEND_ATOMIC_PTR_INIT(&reg->slots[i], NULL);
    }

    return reg;
}

bool worker_registry_publish(worker_registry_t *reg, const int idx, worker_inbox_t *inbox)
{
    if (UNEXPECTED(reg == NULL || idx < 0 || idx >= reg->capacity)) {
        return false;
    }

    zend_atomic_ptr_store_ex(&reg->slots[idx], inbox);

    return true;
}

int worker_registry_add(worker_registry_t *reg, worker_inbox_t *inbox)
{
    if (UNEXPECTED(reg == NULL)) {
        return -1;
    }

    int idx = -1;
    tsrm_mutex_lock(reg->admin);

    for (int i = 0; i < reg->capacity; i++) {
        if (zend_atomic_ptr_load_ex(&reg->slots[i]) == NULL) {
            zend_atomic_ptr_store_ex(&reg->slots[i], inbox);
            idx = i;
            break;
        }
    }

    tsrm_mutex_unlock(reg->admin);
    return idx;
}

bool worker_registry_retire(worker_registry_t *reg, const worker_inbox_t *inbox)
{
    if (UNEXPECTED(reg == NULL || inbox == NULL)) {
        return false;
    }

    bool found = false;
    tsrm_mutex_lock(reg->admin);

    for (int i = 0; i < reg->capacity; i++) {
        if (zend_atomic_ptr_load_ex(&reg->slots[i]) == (void *)inbox) {
            zend_atomic_ptr_store_ex(&reg->slots[i], NULL);
            found = true;
            break;
        }
    }

    tsrm_mutex_unlock(reg->admin);
    return found;
}

int worker_registry_capacity(const worker_registry_t *reg)
{
    return reg != NULL ? reg->capacity : 0;
}

int worker_registry_count(const worker_registry_t *reg)
{
    if (reg == NULL) {
        return 0;
    }

    int n = 0;

    for (int i = 0; i < reg->capacity; i++) {
        if (zend_atomic_ptr_load_ex(&reg->slots[i]) != NULL) {
            n++;
        }
    }

    return n;
}

worker_inbox_t *worker_registry_at(const worker_registry_t *reg, const int idx)
{
    if (UNEXPECTED(reg == NULL || idx < 0 || idx >= reg->capacity)) {
        return NULL;
    }

    return (worker_inbox_t *)zend_atomic_ptr_load_ex(&reg->slots[idx]);
}

worker_inbox_t *worker_registry_pick(worker_registry_t *reg)
{
    if (UNEXPECTED(reg == NULL)) {
        return NULL;
    }

    /* Unsigned so the cursor wraps cleanly past INT_MAX. */
    const unsigned start = (unsigned)zend_atomic_int_fetch_add(&reg->rr, 1);

    for (int k = 0; k < reg->capacity; k++) {
        const int i = (int)((start + (unsigned)k) % (unsigned)reg->capacity);
        worker_inbox_t *const inbox =
            (worker_inbox_t *)zend_atomic_ptr_load_ex(&reg->slots[i]);

        if (EXPECTED(inbox != NULL)) {
            return inbox;
        }
    }

    return NULL;
}

worker_inbox_t *worker_registry_least_busy(worker_registry_t *reg,
                                           const int reactor_id, const int n_reactors,
                                           int *out_slot)
{
    if (out_slot != NULL) {
        *out_slot = -1;
    }

    if (UNEXPECTED(reg == NULL)) {
        return NULL;
    }

    const bool owned = reactor_id >= 0 && n_reactors > 1;
    const int  step  = owned ? n_reactors : 1;
    const int  base  = owned ? (reactor_id % n_reactors) : 0;

    if (base >= reg->capacity) {
        return NULL;
    }

    /* Number of owned positions in [base, capacity) stepping by `step`. */
    const int npos = (reg->capacity - base + step - 1) / step;

    /* Rotate the scan origin so an all-idle owned set (every depth 0) spreads
     * homes round-robin instead of always landing on the lowest slot. */
    const unsigned start = (unsigned)zend_atomic_int_fetch_add(&reg->rr, 1);

    worker_inbox_t *best = NULL;
    size_t          best_depth = 0;
    int             best_slot = -1;

    for (int p = 0; p < npos; p++) {
        const int slot = base + (int)((start + (unsigned)p) % (unsigned)npos) * step;
        worker_inbox_t *const inbox =
            (worker_inbox_t *)zend_atomic_ptr_load_ex(&reg->slots[slot]);

        if (UNEXPECTED(inbox == NULL)) {
            continue;
        }

        const size_t depth = worker_inbox_depth(inbox);

        if (best == NULL || depth < best_depth) {
            best       = inbox;
            best_depth = depth;
            best_slot  = slot;
        }

        if (best_depth == 0) {
            break;
        }
    }

    if (best != NULL && out_slot != NULL) {
        *out_slot = best_slot;
    }

    return best;
}

void worker_registry_free(worker_registry_t *reg)
{
    if (reg == NULL) {
        return;
    }

    tsrm_mutex_free(reg->admin);
    pefree(reg->slots, 0);
    pefree(reg, 0);
}
