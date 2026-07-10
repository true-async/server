/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

#ifndef STATS_REGISTRY_H
#define STATS_REGISTRY_H

#include "php_http_server.h"        /* http_server_counters_t */
#include <Zend/zend_atomic.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Statistics registry for the telemetry API (issue #5, Stage A1).
 *
 * A process-wide, contiguous slab of per-worker counter slots, sized to the
 * worker pool at start(). Each worker owns one slot and spins its own counters
 * into it with no atomics (single-thread per worker). The telemetry API walks
 * the slab from any thread and reads the counters directly — no CAS, no
 * per-read lock; a torn 64-bit read at worst reports a momentarily low value,
 * and a slot mid-retire is skipped, so the aggregate is stale by at most one
 * worker. That looseness is deliberate and acceptable for statistics.
 *
 * The admin mutex is taken ONLY when a worker claims or releases a slot (rare,
 * on pool bring-up / rotation); readers never take it. Mirrors the lock-free-
 * read / lock-on-mutate contract of worker_registry (issue #80).
 *
 * The slab is one aligned allocation so all slots sit in one place (the worker
 * count is known up front). Each slot is cache-line aligned so adjacent workers
 * writing their own counters never false-share.
 *
 * Threading: create()/free() on the parent (pool bring-up / teardown, after
 * workers quiesce); claim()/retire() from a worker on its own thread; at()/
 * capacity()/count() from any thread.
 */

/* Cache-line size for slot padding. 64 B covers x86-64 and arm64. */
#define HTTP_STATS_CACHELINE 64

/* One per-worker counter slot. `counters` is the same slice a running server
 * bumps on its hot path (Stage A2 points server->counters at &slot->counters).
 * `active` gates a slot into the aggregate: claim zeroes the counters and then
 * release-stores active=1; a reader acquire-loads active before summing. */
typedef struct {
    _Alignas(HTTP_STATS_CACHELINE)
    zend_atomic_int        active;      /* 1 = claimed & live, 0 = free */
    int                    worker_id;   /* slot index, used as the label */
    http_server_counters_t counters;    /* per-worker hot counter slice */
} http_stats_slot_t;

typedef struct http_stats_registry_s http_stats_registry_t;

/* Create a registry with `capacity` slots (>= 1). Parent thread. NULL on a bad
 * argument or allocation failure. */
http_stats_registry_t *http_stats_registry_create(int capacity);

/* Claim a free slot: zero its counters, stamp worker_id, mark it active
 * (release). Returns the slot index, or -1 if the slab is full. Worker thread,
 * serialised against other claim/retire by the admin mutex. */
int http_stats_registry_claim(http_stats_registry_t *reg);

/* Release slot `idx` (mark inactive). After this returns the slot is skipped by
 * new reads and reclaimable by a later claim. Worker thread. Returns false for
 * an out-of-range index. */
bool http_stats_registry_retire(http_stats_registry_t *reg, int idx);

/* Total slot count, and number currently active. */
int http_stats_registry_capacity(const http_stats_registry_t *reg);
int http_stats_registry_count(const http_stats_registry_t *reg);

/* Slot at `idx` (for the worker to write and the reader to sum), or NULL when
 * out of range. Any thread. */
http_stats_slot_t *http_stats_registry_at(http_stats_registry_t *reg, int idx);

/* True if the slot is claimed & live (acquire load). Any thread. */
bool http_stats_slot_active(const http_stats_slot_t *slot);

/* Free the slab. Parent thread, after workers quiesce. */
void http_stats_registry_free(http_stats_registry_t *reg);

#ifdef __cplusplus
}
#endif

#endif /* STATS_REGISTRY_H */
