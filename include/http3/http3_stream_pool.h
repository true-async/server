#ifndef HTTP3_STREAM_POOL_H
#define HTTP3_STREAM_POOL_H

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#ifdef HAVE_HTTP_SERVER_HTTP3

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
 * Lifetime: pool is embedded in http3_listener_t. Cleanup at listener
 * teardown asserts no slot is still alive.
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
} http3_stream_pool_t;

void http3_stream_pool_init(http3_stream_pool_t *pool);

/* Frees all chunks. Caller must have released every alive slot first
 * (assert live_count == 0 in debug). */
void http3_stream_pool_cleanup(http3_stream_pool_t *pool);

http3_stream_t *http3_stream_pool_alloc(http3_stream_pool_t *pool);

/* Push a slot back onto the freelist. Caller must have torn down all
 * dependent state (request, body buffers, zvals, etc.) — pool only
 * does the link/unlink. */
void http3_stream_pool_free(http3_stream_pool_t *pool, http3_stream_t *slot);

#ifdef __cplusplus
}
#endif

#endif /* HAVE_HTTP_SERVER_HTTP3 */

#endif /* HTTP3_STREAM_POOL_H */
