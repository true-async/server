/*
 * conn_arena — slab allocator for http_connection_t.
 *
 * Memory model
 * ------------
 *
 *   chunks ─→ [chunk #0: 256 slots] ─→ [chunk #1: 256 slots] ─→ NULL
 *                  ↑                       (chained at chunk header)
 *
 *   free_head ──→ slot ── slot.next_conn ──→ slot ── ... ──→ NULL
 *      (single-linked through `next_conn` while the slot is FREE)
 *
 *   alive_head ←→ slot ←→ slot ←→ slot ←→ ... (doubly-linked)
 *      (linked through `next_conn` / `prev_conn` while the slot is ALIVE)
 *
 * Each slot is a full http_connection_t. Its `next_conn` / `prev_conn`
 * fields play a dual role:
 *   - while the slot is on the freelist, only `next_conn` is meaningful
 *     (single-linked freelist link), the rest of the slot is undefined.
 *   - while the slot is on the alive list, `next_conn` / `prev_conn`
 *     are doubly-linked alive-list links and all other fields are
 *     valid per http_connection_t's contract.
 *
 * Lifetime contract
 * -----------------
 *
 * The arena is embedded in the refcounted http_server_object C-state.
 * Allocating a slot also takes one ref on the server (via the caller —
 * http_connection_create). Freeing a slot drops that ref. cleanup()
 * frees the slab chunks themselves and asserts the alive list is
 * empty — that point is reached only after every live conn has been
 * destroyed, which is the same condition that brings the refcount to
 * zero in http_server_release.
 *
 * Thread-safety: per-worker / single-thread. No locking.
 */
#ifndef CONN_ARENA_H
#define CONN_ARENA_H

#include "http_connection.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Slots per chunk. 256 × ~768 B ≈ 192 KiB. Power-of-two for cleaner
 * grow-by-doubling later if we want; otherwise irrelevant. */
#define CONN_ARENA_CHUNK_SLOTS  256

typedef struct conn_chunk_s {
    struct conn_chunk_s *next_chunk;
    http_connection_t    slots[CONN_ARENA_CHUNK_SLOTS];
} conn_chunk_t;

typedef struct conn_arena_s {
    conn_chunk_t      *chunks;       /* chained slab chunks */
    http_connection_t *free_head;    /* freelist (via slot->next_conn) */
    http_connection_t *alive_head;   /* alive-list (doubly linked) */
    size_t             live_count;
    size_t             slot_count;   /* total slots across all chunks */
} conn_arena_t;

void conn_arena_init(conn_arena_t *arena);

/* Frees all chunks. Caller must have drained the alive list first. */
void conn_arena_cleanup(conn_arena_t *arena);

/* Pop a slot, zero it, push onto alive-list. Grows by one chunk on
 * empty freelist. Returns NULL only on chunk allocation failure. */
http_connection_t *conn_arena_alloc(conn_arena_t *arena);

/* Unlink from alive list, push onto freelist. Caller has already
 * torn down dependent state (io, parser, TLS, ...). */
void conn_arena_free(conn_arena_t *arena, http_connection_t *conn);

#ifdef __cplusplus
}
#endif

#endif /* CONN_ARENA_H */
