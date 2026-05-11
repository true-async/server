#ifndef HTTP_BODY_POOL_H
#define HTTP_BODY_POOL_H

#include <stddef.h>
#include "zend.h"

/* Thread-local LIFO pool of large body buffers, sized in power-of-2
 * classes from 1 MB to 128 MB. Slots themselves are emalloc'd (so they
 * count against memory_limit), but freed slots stay in the per-thread
 * LIFO instead of going back to zend_mm — that's what avoids the
 * mmap_lock contention on hot upload paths. */

#define BODY_POOL_MIN_SIZE        (1u << 20)
#define BODY_POOL_MAX_SIZE        (1u << 27)
#define BODY_POOL_NUM_CLASSES     8
#define BODY_POOL_SLOTS_PER_CLASS 32

bool         body_pool_owns(const zend_string *zstr);
zend_string *body_pool_acquire(size_t len);
void         body_pool_release(zend_string *zstr);

/* Drain all cached slots back to zend_mm. Call from RSHUTDOWN so the
 * debug allocator's leak detector doesn't flag pool-retained slots. */
void         body_pool_shutdown(void);

/* Convenience: release whatever this string is — pool slot via the pool,
 * regular zend_string via zend_string_release. Use this anywhere you'd
 * otherwise call zend_string_release on a request body. */
static inline void body_release(zend_string *zstr)
{
    if (zstr == NULL) return;
    if (body_pool_owns(zstr)) {
        body_pool_release(zstr);
    } else {
        zend_string_release(zstr);
    }
}

#endif
