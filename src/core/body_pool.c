/* Thread-local body buffer pool — keeps freed large bodies in user space
 * to avoid per-request mmap/munmap and the mmap_lock contention that
 * caps multi-worker scaling on upload-heavy workloads. */

#include "body_pool.h"
#include "zend_string.h"
#include <sys/mman.h>
#include <unistd.h>
#include <string.h>

/* Magic stamped into GC_INFO (22 bits) so body_pool_owns() can recognise
 * pool-allocated strings without a side table. */
#define POOL_MAGIC 0x2bf00du

typedef struct {
    zend_string *slots[BODY_POOL_SLOTS_PER_CLASS];
    int          count;
    size_t       capacity;
} body_pool_class_t;

static __thread body_pool_class_t pool_classes[BODY_POOL_NUM_CLASSES];
static __thread bool              pool_initialised = false;

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
        const size_t alloc_size = _ZSTR_STRUCT_SIZE(bucket->capacity);
        void *mem = mmap(NULL, alloc_size,
                         PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS,
                         -1, 0);
        if (mem == MAP_FAILED) return NULL;
        zstr = (zend_string *)mem;
    }

    /* IS_STR_INTERNED makes PHP refcount ops no-ops; lifecycle is owned
     * by us, not the engine. POOL_MAGIC in GC_INFO marks ownership. */
    GC_SET_REFCOUNT(zstr, 1);
    GC_TYPE_INFO(zstr) = GC_STRING
        | ((IS_STR_INTERNED | IS_STR_PERSISTENT) << GC_FLAGS_SHIFT)
        | (POOL_MAGIC << GC_INFO_SHIFT);
    ZSTR_H(zstr)   = 0;
    ZSTR_LEN(zstr) = len;
    ZSTR_VAL(zstr)[len] = '\0';
    return zstr;
}

void body_pool_release(zend_string *zstr)
{
    if (!body_pool_owns(zstr)) return;

    const int cls = size_to_class(ZSTR_LEN(zstr));
    if (cls < 0) {
        const size_t alloc_size = _ZSTR_STRUCT_SIZE(ZSTR_LEN(zstr));
        munmap(zstr, alloc_size);
        return;
    }

    body_pool_class_t *bucket = &pool_classes[cls];
    if (bucket->count < BODY_POOL_SLOTS_PER_CLASS) {
        bucket->slots[bucket->count++] = zstr;
        return;
    }

    const size_t alloc_size = _ZSTR_STRUCT_SIZE(bucket->capacity);
    munmap(zstr, alloc_size);
}
