/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+

  Slab allocator for http3_stream_t. See include/http3/http3_stream_pool.h.
*/

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <php.h>
#include <Zend/zend_alloc.h>
#include <Zend/zend_portability.h>
#include <assert.h>
#include <string.h>

#include "http3/http3_stream_pool.h"

void http3_stream_pool_init(http3_stream_pool_t *pool)
{
    pool->chunks     = NULL;
    pool->free_head  = NULL;
    pool->slot_count = 0;
    pool->live_count = 0;
}

void http3_stream_pool_cleanup(http3_stream_pool_t *pool)
{
    /* Caller contract: every slot returned to the freelist before
     * cleanup. Per-slot teardown happens in http3_stream_release which
     * calls back into http3_stream_pool_free. */
    assert(pool->live_count == 0
           && "http3_stream_pool_cleanup with live slots");

    http3_stream_chunk_t *c = pool->chunks;
    while (c != NULL) {
        http3_stream_chunk_t *next = c->next_chunk;
        efree(c);
        c = next;
    }
    pool->chunks     = NULL;
    pool->free_head  = NULL;
    pool->slot_count = 0;
    pool->live_count = 0;
}

static bool http3_stream_pool_grow(http3_stream_pool_t *pool)
{
    http3_stream_chunk_t *chunk = emalloc(sizeof(*chunk));
    if (UNEXPECTED(chunk == NULL)) {
        return false;
    }
    chunk->next_chunk = pool->chunks;
    pool->chunks = chunk;

    /* Push every slot onto the freelist. Forward order so slot[N-1]
     * ends up at free_head — first allocations come from the tail and
     * reuse cache lines we just touched while linking. Same trick as
     * core/conn_arena. */
    for (size_t i = 0; i < HTTP3_STREAM_POOL_CHUNK_SLOTS; i++) {
        http3_stream_t *slot = &chunk->slots[i];
        slot->list_next = pool->free_head;
        pool->free_head = slot;
    }
    pool->slot_count += HTTP3_STREAM_POOL_CHUNK_SLOTS;
    return true;
}

http3_stream_t *http3_stream_pool_alloc(http3_stream_pool_t *pool)
{
    if (UNEXPECTED(pool->free_head == NULL)) {
        if (!http3_stream_pool_grow(pool)) {
            return NULL;
        }
    }

    http3_stream_t *slot = pool->free_head;
    pool->free_head = slot->list_next;

    /* Wipe — http3_stream_new relies on zero-init for refcount, flags,
     * pointer fields. The pool preserves that contract. */
    memset(slot, 0, sizeof(*slot));
    pool->live_count++;
    return slot;
}

void http3_stream_pool_free(http3_stream_pool_t *pool, http3_stream_t *slot)
{
    /* Push onto freelist. list_next is the only field we set — the
     * rest of the slot is undefined storage. Caller has already torn
     * down per-stream state via http3_stream_release. */
    slot->list_next = pool->free_head;
    pool->free_head = slot;
    pool->live_count--;
}
