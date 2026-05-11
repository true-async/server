#ifndef HTTP_BODY_POOL_H
#define HTTP_BODY_POOL_H

#include <stddef.h>
#include "zend.h"

/* Thread-local LIFO pool of large body buffers, sized in power-of-2
 * classes from 1 MB to 128 MB. Bypasses zend_mm_alloc_huge for bodies
 * in this range to avoid mmap_lock contention under load. */

#define BODY_POOL_MIN_SIZE        (1u << 20)
#define BODY_POOL_MAX_SIZE        (1u << 27)
#define BODY_POOL_NUM_CLASSES     8
#define BODY_POOL_SLOTS_PER_CLASS 8

bool         body_pool_owns(const zend_string *zstr);
zend_string *body_pool_acquire(size_t len);
void         body_pool_release(zend_string *zstr);

#endif
