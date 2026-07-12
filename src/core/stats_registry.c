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

/* The counter field table, materialised. See HTTP_SERVER_COUNTER_TABLE. */
static const struct {
    const char *name;
    size_t      offset;
    int         kind;
} stats_fields[] = {
#define HTTP_COUNTER_ROW(field, k) \
    { #field, offsetof(http_server_counters_t, field), HTTP_COUNTER_##k },
    HTTP_SERVER_COUNTER_TABLE(HTTP_COUNTER_ROW)
#undef HTTP_COUNTER_ROW
};

#define STATS_FIELD_COUNT (sizeof(stats_fields) / sizeof(stats_fields[0]))

/* Fails the build if a counter was added to the struct but not to the table, or
 * if one stopped being a 64-bit word (a 32-bit field would shift every counter
 * after it in the aggregate — the bug this assert exists to prevent). */
ZEND_STATIC_ASSERT(STATS_FIELD_COUNT * sizeof(uint64_t)
                       == sizeof(http_server_counters_t),
                   "HTTP_SERVER_COUNTER_TABLE is out of sync with "
                   "http_server_counters_t");

/* A live worker keeps writing its slot while we read it from another thread.
 * The looseness is deliberate — a statistic may lag a bump — but it has to be
 * spelled as a relaxed atomic load: a plain load racing a plain store is a data
 * race the compiler may reload or tear. Writers stay plain; one writer a slot. */
static zend_always_inline uint64_t stats_field_load(const http_server_counters_t *c,
                                                    size_t offset)
{
    const uint64_t *p = (const uint64_t *)((const char *)c + offset);

    return __atomic_load_n(p, __ATOMIC_RELAXED);
}

size_t http_stats_field_count(void)
{
    return STATS_FIELD_COUNT;
}

const char *http_stats_field_name(size_t i)
{
    return i < STATS_FIELD_COUNT ? stats_fields[i].name : NULL;
}

uint64_t http_stats_field_get(const http_server_counters_t *c, size_t i)
{
    return i < STATS_FIELD_COUNT ? stats_field_load(c, stats_fields[i].offset) : 0;
}

struct http_stats_registry_s {
    http_stats_slot_t *slots;      /* [capacity], one cache-line-aligned slab */
    MUTEX_T            admin;       /* serialises claim/retire; reads stay lock-free */
    int                capacity;
    /* Monotonic totals inherited from workers that have already exited. A slot
     * is recycled by memset at claim, so without this a pool reload would reset
     * every total to zero and getStats() would report a counter running
     * backwards. Only SUM fields land here — see HTTP_SERVER_COUNTER_TABLE.
     * Written under `admin` at retire; read under `admin` in totals(). */
    http_server_counters_t retired;
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

    http_stats_registry_t *const reg = pecalloc(1, sizeof(*reg), 1);
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

    /* Inherit the departing worker's monotonic totals before the slot is freed
     * for recycling; its gauges and samples die with it. The worker is gone by
     * now, so a plain read of its slot races nobody. */
    const http_server_counters_t *c = &reg->slots[idx].counters;

    for (size_t i = 0; i < STATS_FIELD_COUNT; i++) {
        if (stats_fields[i].kind != HTTP_COUNTER_SUM) {
            continue;
        }

        const size_t off = stats_fields[i].offset;
        *(uint64_t *)((char *)&reg->retired + off) +=
            *(const uint64_t *)((const char *)c + off);
    }

    zend_atomic_int_store_ex(&reg->slots[idx].active, 0);
    tsrm_mutex_unlock(reg->admin);

    return true;
}

void http_stats_registry_totals(http_stats_registry_t *reg,
                                http_server_counters_t *out)
{
    memset(out, 0, sizeof *out);

    if (UNEXPECTED(reg == NULL)) {
        return;
    }

    /* Retired totals are read under the same lock that retire() writes them. */
    tsrm_mutex_lock(reg->admin);
    memcpy(out, &reg->retired, sizeof *out);
    tsrm_mutex_unlock(reg->admin);

    for (int s = 0; s < reg->capacity; s++) {
        const http_stats_slot_t *slot = &reg->slots[s];

        if (!http_stats_slot_active(slot)) {
            continue;
        }

        for (size_t i = 0; i < STATS_FIELD_COUNT; i++) {
            const size_t   off = stats_fields[i].offset;
            uint64_t      *dst = (uint64_t *)((char *)out + off);
            const uint64_t v   = stats_field_load(&slot->counters, off);

            if (stats_fields[i].kind == HTTP_COUNTER_MAX) {
                if (v > *dst) {
                    *dst = v;
                }
            } else {
                *dst += v;   /* SUM and GAUGE both add across live workers */
            }
        }
    }
}

void http_stats_counters_add(http_server_counters_t *acc,
                             const http_server_counters_t *c)
{
    for (size_t i = 0; i < STATS_FIELD_COUNT; i++) {
        const size_t   off = stats_fields[i].offset;
        uint64_t      *dst = (uint64_t *)((char *)acc + off);
        const uint64_t v   = stats_field_load(c, off);

        if (stats_fields[i].kind == HTTP_COUNTER_MAX) {
            if (v > *dst) {
                *dst = v;
            }
        } else {
            *dst += v;
        }
    }
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
