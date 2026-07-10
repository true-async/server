/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+

  Statistics registry (issue #5, A1) — contiguous per-worker counter slab.
  See include/core/stats_registry.h.
*/

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include "php.h"
#include "TSRM.h"
#include "Zend/zend_atomic.h"
#include "core/stats_registry.h"

#include <stdlib.h>
#include <string.h>
#ifdef PHP_WIN32
# include <malloc.h>
#endif

struct http_stats_registry_s {
    http_stats_slot_t *slots;      /* [capacity], one cache-line-aligned slab */
    MUTEX_T            admin;       /* serialises claim/retire; reads stay lock-free */
    int                capacity;
};

/* sizeof(http_stats_slot_t) is a multiple of HTTP_STATS_CACHELINE (the _Alignas
 * on the slot), so a slab of `capacity` slots is already a multiple of the
 * alignment — the precondition aligned_alloc() requires. */
static http_stats_slot_t *stats_slab_alloc(const int capacity)
{
    const size_t bytes = (size_t)capacity * sizeof(http_stats_slot_t);

#ifdef PHP_WIN32
    http_stats_slot_t *const slab =
        (http_stats_slot_t *)_aligned_malloc(bytes, HTTP_STATS_CACHELINE);
#else
    http_stats_slot_t *const slab =
        (http_stats_slot_t *)aligned_alloc(HTTP_STATS_CACHELINE, bytes);
#endif

    if (slab == NULL) {
        return NULL;
    }

    /* Zeroing the slab initialises every slot's active flag to 0 (free) and its
     * counters to 0 — ZEND_ATOMIC_INT_INIT(&x, 0) is just x.value = 0. */
    memset(slab, 0, bytes);

    return slab;
}

static void stats_slab_free(http_stats_slot_t *slab)
{
#ifdef PHP_WIN32
    _aligned_free(slab);
#else
    free(slab);
#endif
}

http_stats_registry_t *http_stats_registry_create(const int capacity)
{
    if (capacity <= 0) {
        return NULL;
    }

    http_stats_slot_t *const slab = stats_slab_alloc(capacity);

    if (slab == NULL) {
        return NULL;
    }

    http_stats_registry_t *const reg = pemalloc(sizeof(*reg), 1);
    reg->slots    = slab;
    reg->capacity = capacity;
    reg->admin    = tsrm_mutex_alloc();

    return reg;
}

int http_stats_registry_claim(http_stats_registry_t *reg)
{
    if (UNEXPECTED(reg == NULL)) {
        return -1;
    }

    int idx = -1;
    tsrm_mutex_lock(reg->admin);

    for (int i = 0; i < reg->capacity; i++) {
        if (zend_atomic_int_load_ex(&reg->slots[i].active) != 0) {
            continue;
        }

        /* Zero the counters (a rotated slot may hold a dead worker's totals)
         * and stamp the label BEFORE publishing active=1, so a reader that sees
         * the slot live never sums stale counts. */
        memset(&reg->slots[i].counters, 0, sizeof(reg->slots[i].counters));
        reg->slots[i].worker_id = i;
        zend_atomic_int_store_ex(&reg->slots[i].active, 1);
        idx = i;
        break;
    }

    tsrm_mutex_unlock(reg->admin);
    return idx;
}

bool http_stats_registry_retire(http_stats_registry_t *reg, const int idx)
{
    if (UNEXPECTED(reg == NULL || idx < 0 || idx >= reg->capacity)) {
        return false;
    }

    tsrm_mutex_lock(reg->admin);
    zend_atomic_int_store_ex(&reg->slots[idx].active, 0);
    tsrm_mutex_unlock(reg->admin);

    return true;
}

int http_stats_registry_capacity(const http_stats_registry_t *reg)
{
    return reg != NULL ? reg->capacity : 0;
}

int http_stats_registry_count(const http_stats_registry_t *reg)
{
    if (reg == NULL) {
        return 0;
    }

    int n = 0;

    for (int i = 0; i < reg->capacity; i++) {
        if (zend_atomic_int_load_ex(&reg->slots[i].active) != 0) {
            n++;
        }
    }

    return n;
}

http_stats_slot_t *http_stats_registry_at(http_stats_registry_t *reg, const int idx)
{
    if (UNEXPECTED(reg == NULL || idx < 0 || idx >= reg->capacity)) {
        return NULL;
    }

    return &reg->slots[idx];
}

bool http_stats_slot_active(const http_stats_slot_t *slot)
{
    if (UNEXPECTED(slot == NULL)) {
        return false;
    }

    /* zend_atomic_int_load_ex is non-const on the Windows Interlocked path;
     * the load never mutates, so casting away const here is sound. */
    return zend_atomic_int_load_ex((zend_atomic_int *)&slot->active) != 0;
}

void http_stats_registry_free(http_stats_registry_t *reg)
{
    if (reg == NULL) {
        return;
    }

    tsrm_mutex_free(reg->admin);
    stats_slab_free(reg->slots);
    pefree(reg, 1);
}
