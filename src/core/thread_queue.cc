/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

/*
 * C-ABI shims over the moodycamel lock-free queues (issue #81). See
 * include/core/thread_queue.h for the contract and deps/concurrentqueue/
 * for why these two queues were chosen.
 *
 * Boundedness is layered on top of the underlying (effectively unbounded)
 * moodycamel queues: an atomic length counter is the authoritative cap.
 * A producer reserves a slot in the counter first; only on success does it
 * push. This gives a deterministic high-water mark, drives the empty->non-
 * empty edge used for wakeup, and means we never allocate past the cap.
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include "core/thread_queue.h"

#if !defined(__cplusplus) || __cplusplus < 201103L
# error "thread_queue.cc requires a C++11 compiler"
#endif

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <new>
#if defined(_WIN32)
# include <malloc.h>
#endif

/* moodycamel headers are vendored third-party; quiet their warnings under
 * our -Wall -Wextra without touching upstream. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include "concurrentqueue.h"
#include "readerwriterqueue.h"
#pragma GCC diagnostic pop

namespace {

/* The wrapper structs embed cache-line-aligned moodycamel members, so they are
 * over-aligned (alignof 64). Plain operator new only guarantees max_align_t
 * before C++17; route allocation through an aligned allocator so the padding
 * actually lands on cache-line boundaries on every standard. */
inline void *aligned_new(const size_t size, size_t align)
{
    if (align < sizeof(void *)) {
        align = sizeof(void *);
    }

#if defined(_WIN32)
    void *p = _aligned_malloc(size, align);
#else
    void *p = nullptr;
    if (posix_memalign(&p, align, size) != 0) {
        p = nullptr;
    }
#endif

    if (p == nullptr) {
        throw std::bad_alloc();
    }

    return p;
}

inline void aligned_delete(void *p)
{
#if defined(_WIN32)
    _aligned_free(p);
#else
    free(p);
#endif
}

/* Reserve one slot against the cap. Returns true and reports the pre-increment
 * count in *prev when there was room; false (no change) when full. Works for a
 * single or multiple producers — the CAS loop simply never contends in the
 * single-producer case. */
inline bool cap_reserve(std::atomic<size_t> &count, const size_t capacity, size_t &prev)
{
    prev = count.load(std::memory_order_relaxed);
    do {
        if (prev >= capacity) {
            return false;
        }
    } while (!count.compare_exchange_weak(
            prev, prev + 1, std::memory_order_acq_rel, std::memory_order_relaxed));

    return true;
}

} // namespace

/* ------------------------------------------------------------------ */
/* MPSC — moodycamel::ConcurrentQueue                                 */
/* ------------------------------------------------------------------ */

struct thread_mpsc_s {
    moodycamel::ConcurrentQueue<void *> q;
    std::atomic<size_t>                 count;
    size_t                              capacity;

    explicit thread_mpsc_s(const size_t cap) : q(cap), count(0), capacity(cap) {}

    static void *operator new(const size_t sz) { return aligned_new(sz, alignof(thread_mpsc_s)); }
    static void  operator delete(void *p) noexcept { aligned_delete(p); }
};

thread_mpsc_t *thread_mpsc_create(const size_t capacity)
{
    if (capacity == 0) {
        return nullptr;
    }

    try {
        return new thread_mpsc_s(capacity);
    } catch (...) {
        return nullptr;
    }
}

void thread_mpsc_free(thread_mpsc_t *q)
{
    delete q;
}

bool thread_mpsc_enqueue(thread_mpsc_t *q, void *item, bool *was_empty)
{
    size_t prev;
    if (!cap_reserve(q->count, q->capacity, prev)) {
        return false;
    }

    if (!q->q.try_enqueue(item)) {
        q->count.fetch_sub(1, std::memory_order_release);
        return false;
    }

    if (was_empty != nullptr) {
        *was_empty = (prev == 0);
    }

    return true;
}

bool thread_mpsc_dequeue(thread_mpsc_t *q, void **item)
{
    if (q->q.try_dequeue(*item)) {
        q->count.fetch_sub(1, std::memory_order_acq_rel);
        return true;
    }

    return false;
}

size_t thread_mpsc_drain(thread_mpsc_t *q, void **items, const size_t max)
{
    if (max == 0) {
        return 0;
    }

    const size_t n = q->q.try_dequeue_bulk(items, max);
    if (n != 0) {
        q->count.fetch_sub(n, std::memory_order_acq_rel);
    }

    return n;
}

size_t thread_mpsc_count(const thread_mpsc_t *q)
{
    return q->count.load(std::memory_order_acquire);
}

/* ------------------------------------------------------------------ */
/* SPSC — moodycamel::ReaderWriterQueue                               */
/* ------------------------------------------------------------------ */

struct thread_spsc_s {
    moodycamel::ReaderWriterQueue<void *> q;
    std::atomic<size_t>                   count;
    size_t                                capacity;

    explicit thread_spsc_s(const size_t cap) : q(cap), count(0), capacity(cap) {}

    static void *operator new(const size_t sz) { return aligned_new(sz, alignof(thread_spsc_s)); }
    static void  operator delete(void *p) noexcept { aligned_delete(p); }
};

thread_spsc_t *thread_spsc_create(const size_t capacity)
{
    if (capacity == 0) {
        return nullptr;
    }

    try {
        return new thread_spsc_s(capacity);
    } catch (...) {
        return nullptr;
    }
}

void thread_spsc_free(thread_spsc_t *q)
{
    delete q;
}

bool thread_spsc_enqueue(thread_spsc_t *q, void *item, bool *was_empty)
{
    size_t prev;
    if (!cap_reserve(q->count, q->capacity, prev)) {
        return false;
    }

    if (!q->q.try_enqueue(item)) {
        q->count.fetch_sub(1, std::memory_order_release);
        return false;
    }

    if (was_empty != nullptr) {
        *was_empty = (prev == 0);
    }

    return true;
}

bool thread_spsc_dequeue(thread_spsc_t *q, void **item)
{
    if (q->q.try_dequeue(*item)) {
        q->count.fetch_sub(1, std::memory_order_acq_rel);
        return true;
    }

    return false;
}

size_t thread_spsc_drain(thread_spsc_t *q, void **items, const size_t max)
{
    size_t n = 0;
    while (n < max && q->q.try_dequeue(items[n])) {
        ++n;
    }

    if (n != 0) {
        q->count.fetch_sub(n, std::memory_order_acq_rel);
    }

    return n;
}

size_t thread_spsc_count(const thread_spsc_t *q)
{
    return q->count.load(std::memory_order_acquire);
}
