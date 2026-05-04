/*
 * conn_arena implementation. See conn_arena.h.
 */
#include "conn_arena.h"

#include <Zend/zend_alloc.h>
#include <Zend/zend_portability.h>
#include <assert.h>
#include <string.h>

void conn_arena_init(conn_arena_t *arena)
{
    arena->chunks     = NULL;
    arena->free_head  = NULL;
    arena->alive_head = NULL;
    arena->live_count = 0;
    arena->slot_count = 0;
}

void conn_arena_cleanup(conn_arena_t *arena)
{
    /* Caller contract: arena_free must have drained the alive list
     * before cleanup. We only release slab memory here; per-slot
     * teardown happens in http_connection_destroy. */
    assert(arena->alive_head == NULL && "conn_arena_cleanup with live conns");
    assert(arena->live_count == 0    && "conn_arena_cleanup with live_count > 0");

    /* Slab itself lives in pemalloc on the persistent heap (the C-state
     * is pemalloc'd, the arena is embedded in it). Use pefree to match. */
    conn_chunk_t *c = arena->chunks;
    while (c != NULL) {
        conn_chunk_t *next = c->next_chunk;
        pefree(c, /*persistent*/ 1);
        c = next;
    }
    arena->chunks     = NULL;
    arena->free_head  = NULL;
    arena->slot_count = 0;
}

/* Grow by one chunk. Returns false on alloc failure. */
static bool conn_arena_grow(conn_arena_t *arena)
{
    conn_chunk_t *chunk = pemalloc(sizeof(*chunk), /*persistent*/ 1);
    if (UNEXPECTED(chunk == NULL)) {
        return false;
    }
    chunk->next_chunk = arena->chunks;
    arena->chunks = chunk;

    /* Push every slot onto the freelist. We do this in forward order
     * so slot[N-1] ends up at free_head — the first allocations come
     * from the high end of the chunk and reuse the cache lines we
     * just touched while initialising the chain. */
    for (size_t i = 0; i < CONN_ARENA_CHUNK_SLOTS; i++) {
        http_connection_t *slot = &chunk->slots[i];
        slot->next_conn = arena->free_head;
        arena->free_head = slot;
    }
    arena->slot_count += CONN_ARENA_CHUNK_SLOTS;
    return true;
}

http_connection_t *conn_arena_alloc(conn_arena_t *arena)
{
    if (UNEXPECTED(arena->free_head == NULL)) {
        if (!conn_arena_grow(arena)) {
            return NULL;
        }
    }

    http_connection_t *slot = arena->free_head;
    arena->free_head = slot->next_conn;

    /* Wipe — http_connection_create relies on zero-init for flags and
     * pointer fields; the arena preserves that contract. */
    memset(slot, 0, sizeof(*slot));

    slot->next_conn = arena->alive_head;
    slot->prev_conn = NULL;
    if (arena->alive_head != NULL) {
        arena->alive_head->prev_conn = slot;
    }
    arena->alive_head = slot;
    arena->live_count++;
    return slot;
}

void conn_arena_free(conn_arena_t *arena, http_connection_t *conn)
{
    if (conn->prev_conn != NULL) {
        conn->prev_conn->next_conn = conn->next_conn;
    } else {
        assert(arena->alive_head == conn);
        arena->alive_head = conn->next_conn;
    }
    if (conn->next_conn != NULL) {
        conn->next_conn->prev_conn = conn->prev_conn;
    }
    arena->live_count--;

    /* prev_conn is left undefined on the freelist — only next_conn is read. */
    conn->next_conn = arena->free_head;
    arena->free_head = conn;
}
