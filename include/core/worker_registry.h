/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

#ifndef WORKER_REGISTRY_H
#define WORKER_REGISTRY_H

#include <stdbool.h>
#include "core/worker_inbox.h"

/*
 * Worker registry for the reactor/worker split (issue #80, B3 — producer side).
 *
 * A fixed-size table of per-worker inboxes (worker_inbox.h). The parent creates
 * it sized for the worker pool; each worker publishes its own inbox into its
 * slot at startup; a transport reactor reads the table to choose which worker
 * to hand a parsed request to. Publication is a single atomic store, lookup a
 * single atomic load — no lock on the dispatch path.
 *
 * Dispatch (D5): reactor-paired sticky-default with load spill. Each reactor owns a
 * strided subset of slots; worker_registry_least_busy picks the least-loaded owned
 * worker (ties rotate so idle connections spread). The H3 path homes a connection to
 * one worker and reuses it, spilling a request to a less-loaded worker — owned, else
 * any (reactor_id < 0) — when the home backs up or dies. worker_registry_pick (flat
 * round-robin) stays for the unit-test path.
 *
 * Threading: create()/free() on the parent; publish() once per worker on its
 * own thread (release); pick()/at()/count() from any thread (acquire). free()
 * after producers and workers have quiesced — it frees the table, not the
 * inboxes (each worker frees its own).
 */

typedef struct worker_registry_s worker_registry_t;

/* Create a registry with `capacity` slots (>= 1). Parent thread. NULL on bad
 * argument or allocation failure. */
worker_registry_t *worker_registry_create(int capacity);

/* Publish `inbox` at slot `idx` (release store). The worker calls this on its
 * own thread once its inbox is up. Returns false for an out-of-range slot. */
bool worker_registry_publish(worker_registry_t *reg, int idx, worker_inbox_t *inbox);

/* Atomically claim the next free slot and publish `inbox` into it — lets each
 * worker register without being told its index. Returns the slot index, or -1
 * if the table is full. Any thread. */
int worker_registry_add(worker_registry_t *reg, worker_inbox_t *inbox);

/* Number of slots, and number currently published. */
int worker_registry_capacity(const worker_registry_t *reg);
int worker_registry_count(const worker_registry_t *reg);

/* Inbox published at slot `idx` (sticky lookup), or NULL if unpublished /
 * out of range. */
worker_inbox_t *worker_registry_at(const worker_registry_t *reg, int idx);

/* Next published inbox, round-robin across slots (atomic counter). NULL when no
 * slot is published yet. Any thread. */
worker_inbox_t *worker_registry_pick(worker_registry_t *reg);

/* Least-loaded published inbox for a reactor, by worker_inbox_depth. Ownership
 * is strided: reactor `reactor_id` owns slots {i : i % n_reactors == reactor_id}.
 * `reactor_id` < 0 (or `n_reactors` <= 1) scans ALL slots — the global spill / fallback.
 * Ties rotate via the round-robin counter so connections homing while idle spread
 * across the owned set. Writes the chosen slot to *out_slot (-1 if none). Any thread. */
worker_inbox_t *worker_registry_least_busy(worker_registry_t *reg,
                                           int reactor_id, int n_reactors,
                                           int *out_slot);

/* Free the table (not the inboxes). Parent thread, after workers quiesce. */
void worker_registry_free(worker_registry_t *reg);

#endif /* WORKER_REGISTRY_H */
