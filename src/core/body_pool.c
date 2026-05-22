/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

/* Thread-local pool of large body buffers. Allocations go through
 * zend_mm (emalloc → zend_mm_alloc_huge → single mmap), so memory_limit
 * and peak-usage tracking work the same way they do for any other
 * zend_string. The win is reuse: freed buffers stay in the per-thread
 * LIFO instead of being munmap'd, so subsequent requests don't take the
 * mmap_lock. */

#include "body_pool.h"
#include "php.h"
#include "zend_string.h"

/* Magic stamped into GC_INFO (22 bits) so body_pool_owns() can recognise
 * pool-allocated strings without a side table. */
#define POOL_MAGIC 0x2bf00du

typedef struct {
    zend_string *slots[BODY_POOL_SLOTS_PER_CLASS];
    int          count;
    size_t       capacity;
} body_pool_class_t;

/* ZEND_TLS expands to "static TSRM_TLS" (i.e., static + __declspec(thread)
 * on MSVC, or static + __thread on GCC). Do not add another "static". */
ZEND_TLS body_pool_class_t pool_classes[BODY_POOL_NUM_CLASSES];
ZEND_TLS bool              pool_initialised = false;

static inline int size_to_class(const size_t len)
{
    if (len <= BODY_POOL_MIN_SIZE) return 0;
    if (len > BODY_POOL_MAX_SIZE)  return -1;
    int cls = 0;
    size_t cap = BODY_POOL_MIN_SIZE;
    while (cap < len) { cap <<= 1; cls++; }
    return cls;
}

static inline void pool_init_once(void)
{
    if (pool_initialised) return;
    size_t cap = BODY_POOL_MIN_SIZE;
    for (int i = 0; i < BODY_POOL_NUM_CLASSES; i++) {
        pool_classes[i].count    = 0;
        pool_classes[i].capacity = cap;
        cap <<= 1;
    }
    pool_initialised = true;
}

bool body_pool_owns(const zend_string *zstr)
{
    if (zstr == NULL || !ZSTR_IS_INTERNED(zstr)) return false;
    return ((GC_TYPE_INFO(zstr) & GC_INFO_MASK) >> GC_INFO_SHIFT) == POOL_MAGIC;
}

zend_string *body_pool_acquire(const size_t len)
{
    pool_init_once();

    const int cls = size_to_class(len);
    if (cls < 0) return NULL;

    body_pool_class_t *bucket = &pool_classes[cls];
    zend_string *zstr;

    if (bucket->count > 0) {
        zstr = bucket->slots[--bucket->count];
    } else {
        /* Allocate a fresh slot via zend_mm. emalloc honours memory_limit
         * and goes through zend_mm_alloc_huge for sizes >= ZEND_MM_CHUNK_SIZE,
         * which is exactly one mmap. The zend_try guard catches the longjmp
         * raised on OOM so we can fall back to the caller's normal path. */
        const size_t alloc_size = _ZSTR_STRUCT_SIZE(bucket->capacity);
        zend_string *new_zstr = NULL;
        zend_try {
            new_zstr = (zend_string *)emalloc(alloc_size);
        } zend_catch {
            new_zstr = NULL;
        } zend_end_try();
        if (new_zstr == NULL) return NULL;
        zstr = new_zstr;
    }

    /* IS_STR_INTERNED makes PHP refcount ops no-ops; lifecycle is owned
     * by us, not the engine. POOL_MAGIC in GC_INFO marks ownership so
     * body_pool_owns() can recognise the slot from any release site. */
    GC_SET_REFCOUNT(zstr, 1);
    GC_TYPE_INFO(zstr) = GC_STRING
        | (IS_STR_INTERNED << GC_FLAGS_SHIFT)
        | (POOL_MAGIC << GC_INFO_SHIFT);
    ZSTR_H(zstr)   = 0;
    ZSTR_LEN(zstr) = len;
    ZSTR_VAL(zstr)[len] = '\0';
    return zstr;
}

void body_pool_release(zend_string *zstr)
{
    if (!body_pool_owns(zstr)) return;

    /* Pool was already drained (RSHUTDOWN/stop) — late-arriving releases
     * (e.g. HttpRequest object dtors during PHP fast-shutdown) must go
     * straight to zend_mm, otherwise they'd repopulate the freelist and
     * leak past the debug allocator's checkpoint. */
    if (!pool_initialised) {
        efree(zstr);
        return;
    }

    const int cls = size_to_class(ZSTR_LEN(zstr));
    if (cls < 0) {
        efree(zstr);
        return;
    }

    body_pool_class_t *bucket = &pool_classes[cls];
    if (bucket->count < BODY_POOL_SLOTS_PER_CLASS) {
        bucket->slots[bucket->count++] = zstr;
        return;
    }

    /* Pool full — return the slot to zend_mm. */
    efree(zstr);
}

void body_pool_shutdown(void)
{
    if (!pool_initialised) return;

    for (int i = 0; i < BODY_POOL_NUM_CLASSES; i++) {
        body_pool_class_t *bucket = &pool_classes[i];

        for (int j = 0; j < bucket->count; j++) {
            efree(bucket->slots[j]);
            bucket->slots[j] = NULL;
        }

        bucket->count = 0;
    }

    pool_initialised = false;
}
