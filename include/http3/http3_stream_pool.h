/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

#ifndef HTTP3_STREAM_POOL_H
#define HTTP3_STREAM_POOL_H

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include "http3/http3_stream.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Slab allocator for http3_stream_t. Same shape as core/conn_arena —
 * chained chunks of N slots each, single-linked freelist via the
 * existing `list_next` field on the stream slot.
 *
 * Memory model
 * ------------
 *
 *   chunks ─→ [chunk #0: 64 slots] ─→ [chunk #1: 64 slots] ─→ NULL
 *
 *   free_head ──→ slot ── slot.list_next ──→ slot ── ... ──→ NULL
 *      (single-linked through `list_next` while the slot is FREE)
 *
 * `list_next` plays a dual role: while the slot is on the freelist it
 * is the freelist link; while the slot is alive it is the per-conn
 * live-stream list link (http3_connection_t::streams_head). The two
 * memberships are mutually exclusive in time.
 *
 * Lifetime: the pool outlives the listener that created it. A slot can stay
 * alive past listener teardown, because a handler may keep the PHP HttpRequest
 * wrapper — in `$GLOBALS`, in a queue, in a VM frame a bailout abandoned — and
 * the slot only returns when that wrapper is collected. The listener therefore
 * closes the pool rather than freeing it, and the last slot to come back frees
 * the chunks and the pool struct together.
 *
 * The slab is persistent memory for that reason. Under the reactor pool the
 * return lands on a thread whose request heap may already be gone, which is
 * longer than ZMM can hold a block.
 *
 * Thread-safety: H3 listener is single-thread per worker. No locking. */

#ifndef HTTP3_STREAM_POOL_CHUNK_SLOTS
# define HTTP3_STREAM_POOL_CHUNK_SLOTS  64
#endif

typedef struct http3_stream_chunk_s {
    struct http3_stream_chunk_s *next_chunk;
    http3_stream_t               slots[HTTP3_STREAM_POOL_CHUNK_SLOTS];
} http3_stream_chunk_t;

typedef struct http3_stream_pool_s {
    http3_stream_chunk_t *chunks;
    http3_stream_t       *free_head;   /* via slot->list_next */
    size_t                slot_count;
    size_t                live_count;
    bool                  closed;      /* creator gone; the last slot back frees the pool */
} http3_stream_pool_t;

/* Where a slot goes home when the request's last reference drops on a worker
 * thread. True means the owning reactor must reclaim it, because the slab is
 * the reactor's and a free from here would mutate its freelist and counters
 * across threads.
 *
 * The answer comes from the flag stamped at creation, not from the stream's
 * connection: teardown NULLs that pointer while a worker still holds the
 * request, which is exactly the population this question is asked about. A
 * closed pool answers false whatever the flag says — its listener is gone, so
 * no reactor takes work for it and nothing can be racing for the slot. */
static inline bool http3_stream_slot_goes_to_reactor(const http3_stream_t *s)
{
    return s->reactor_owned && s->pool != NULL && !s->pool->closed;
}

/* An empty pool, owned by the caller until it closes it. Never NULL: the
 * allocator bails out rather than fail. */
http3_stream_pool_t *http3_stream_pool_create(void);

/* Give up the creator's ownership. The pool and its chunks are freed here when
 * no slot is out, and by the last http3_stream_pool_free otherwise. No slot may
 * be allocated afterwards, and the caller must drop its pointer either way. */
void http3_stream_pool_close(http3_stream_pool_t *pool);

http3_stream_t *http3_stream_pool_alloc(http3_stream_pool_t *pool);

/* Push a slot back onto the freelist. Caller must have torn down all
 * dependent state (request, body buffers, zvals, etc.) — pool only
 * does the link/unlink. Frees @p pool when it is the last slot out of a closed
 * pool, so neither pointer may be read after the call. */
void http3_stream_pool_free(http3_stream_pool_t *pool, http3_stream_t *slot);

#ifdef __cplusplus
}
#endif

#endif /* HTTP3_STREAM_POOL_H */
