/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

#ifndef THREAD_QUEUE_H
#define THREAD_QUEUE_H

#include <stddef.h>
#include <stdbool.h>

/*
 * Lock-free inter-thread message queues carrying a single `void *` payload
 * (issue #81). C-ABI shims over the moodycamel queues picked by the #81
 * benchmark:
 *
 *   thread_mpsc_t  — many producers, one consumer (moodycamel ConcurrentQueue)
 *   thread_spsc_t  — one producer, one consumer  (moodycamel ReaderWriterQueue)
 *
 * Both are *bounded*: a capacity is fixed at creation and enqueue fails cleanly
 * when full, giving deterministic backpressure with no allocation on the hot
 * path (ties into the OOM-firewall philosophy). The bound is enforced by an
 * explicit atomic length counter on top of the underlying queue, which also
 * drives the empty->non-empty edge used for wakeup (see thread_mailbox.h).
 *
 * Ownership of the payload is the caller's: the queue moves the pointer, it
 * neither frees nor dereferences it.
 */

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* MPSC — many producers, single consumer.                            */
/* ------------------------------------------------------------------ */

typedef struct thread_mpsc_s thread_mpsc_t;

/* Create a bounded MPSC queue holding at most `capacity` items.
 * Returns NULL on allocation failure or capacity == 0. */
thread_mpsc_t *thread_mpsc_create(size_t capacity);
void           thread_mpsc_free(thread_mpsc_t *q);

/* Producer side — callable from any thread.
 * Returns true if the item was enqueued, false if the queue is full.
 * When `was_empty` is non-NULL it is set true iff this enqueue took the queue
 * from empty to non-empty (i.e. the caller should signal the consumer). */
bool   thread_mpsc_enqueue(thread_mpsc_t *q, void *item, bool *was_empty);

/* Consumer side — single thread only.
 * Returns true and stores the item if one was available, false if empty. */
bool   thread_mpsc_dequeue(thread_mpsc_t *q, void **item);

/* Consumer side — drain up to `max` items into `items`, returns the count
 * actually dequeued (0 when empty). */
size_t thread_mpsc_drain(thread_mpsc_t *q, void **items, size_t max);

/* Approximate number of queued items. Exact on a quiescent queue. */
size_t thread_mpsc_count(const thread_mpsc_t *q);

/* ------------------------------------------------------------------ */
/* SPSC — single producer, single consumer.                           */
/* ------------------------------------------------------------------ */

typedef struct thread_spsc_s thread_spsc_t;

thread_spsc_t *thread_spsc_create(size_t capacity);
void           thread_spsc_free(thread_spsc_t *q);

bool   thread_spsc_enqueue(thread_spsc_t *q, void *item, bool *was_empty);
bool   thread_spsc_dequeue(thread_spsc_t *q, void **item);
size_t thread_spsc_drain(thread_spsc_t *q, void **items, size_t max);
size_t thread_spsc_count(const thread_spsc_t *q);

/* ------------------------------------------------------------------ */
/* MPSC carrying reactor_cmd_t BY VALUE — many producers, single       */
/* consumer. Same bounded/lock-free contract as the void* MPSC, but    */
/* the fixed-size command POD is stored in the ring itself, so the hot */
/* worker->reactor path enqueues without a per-message malloc.         */
/* ------------------------------------------------------------------ */

typedef struct reactor_cmd_s   reactor_cmd_t;   /* core/reactor_cmd.h */
typedef struct thread_cmd_mpsc_s thread_cmd_mpsc_t;

thread_cmd_mpsc_t *thread_cmd_mpsc_create(size_t capacity);
void               thread_cmd_mpsc_free(thread_cmd_mpsc_t *q);

/* Copies *cmd into the ring. Returns false when full. */
bool   thread_cmd_mpsc_enqueue(thread_cmd_mpsc_t *q, const reactor_cmd_t *cmd);
/* Drains up to `max` commands into `out` (an array of `max` reactor_cmd_t). */
size_t thread_cmd_mpsc_drain(thread_cmd_mpsc_t *q, reactor_cmd_t *out, size_t max);
size_t thread_cmd_mpsc_count(const thread_cmd_mpsc_t *q);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* THREAD_QUEUE_H */
