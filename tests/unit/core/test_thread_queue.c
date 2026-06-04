/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

/* Unit tests for the lock-free inter-thread queues (issue #81). These cover the
 * raw C-ABI layer (thread_queue.h): single-threaded correctness, the bounded
 * cap / backpressure, batch drain, the was_empty edge, and a multi-producer
 * stress run that asserts every item arrives exactly once. */

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>

#include "core/thread_queue.h"

#define PTR(i)   ((void *) (intptr_t) (i))
#define IVAL(p)  ((int) (intptr_t) (p))

/* ------------------------------------------------------------------ */
/* MPSC                                                               */
/* ------------------------------------------------------------------ */

static void test_mpsc_basic(void **state)
{
    (void) state;

    thread_mpsc_t *q = thread_mpsc_create(8);
    assert_non_null(q);
    assert_int_equal(thread_mpsc_count(q), 0);

    bool was_empty = false;
    assert_true(thread_mpsc_enqueue(q, PTR(1), &was_empty));
    assert_true(was_empty); /* 0 -> 1 edge */

    assert_true(thread_mpsc_enqueue(q, PTR(2), &was_empty));
    assert_false(was_empty);

    assert_int_equal(thread_mpsc_count(q), 2);

    /* Single producer => FIFO. */
    void *item = NULL;
    assert_true(thread_mpsc_dequeue(q, &item));
    assert_int_equal(IVAL(item), 1);
    assert_true(thread_mpsc_dequeue(q, &item));
    assert_int_equal(IVAL(item), 2);

    assert_false(thread_mpsc_dequeue(q, &item));
    assert_int_equal(thread_mpsc_count(q), 0);

    /* Drained to empty => next enqueue is an edge again. */
    assert_true(thread_mpsc_enqueue(q, PTR(3), &was_empty));
    assert_true(was_empty);

    thread_mpsc_free(q);
}

static void test_mpsc_bound(void **state)
{
    (void) state;

    thread_mpsc_t *q = thread_mpsc_create(2);
    assert_non_null(q);

    assert_true(thread_mpsc_enqueue(q, PTR(1), NULL));
    assert_true(thread_mpsc_enqueue(q, PTR(2), NULL));

    /* Full => clean rejection, no growth. */
    assert_false(thread_mpsc_enqueue(q, PTR(3), NULL));
    assert_int_equal(thread_mpsc_count(q), 2);

    /* Freeing a slot re-admits. */
    void *item = NULL;
    assert_true(thread_mpsc_dequeue(q, &item));
    assert_true(thread_mpsc_enqueue(q, PTR(3), NULL));
    assert_int_equal(thread_mpsc_count(q), 2);

    thread_mpsc_free(q);
}

static void test_mpsc_drain(void **state)
{
    (void) state;

    thread_mpsc_t *q = thread_mpsc_create(64);
    assert_non_null(q);

    for (int i = 1; i <= 10; i++) {
        assert_true(thread_mpsc_enqueue(q, PTR(i), NULL));
    }

    void  *batch[4];
    size_t total = 0;
    int    sum   = 0;
    for (;;) {
        const size_t n = thread_mpsc_drain(q, batch, 4);
        if (n == 0) {
            break;
        }

        assert_true(n <= 4);
        for (size_t i = 0; i < n; i++) {
            sum += IVAL(batch[i]);
        }
        total += n;
    }

    assert_int_equal(total, 10);
    assert_int_equal(sum, 55); /* 1+..+10 */
    assert_int_equal(thread_mpsc_count(q), 0);

    thread_mpsc_free(q);
}

/* ------------------------------------------------------------------ */
/* SPSC                                                               */
/* ------------------------------------------------------------------ */

static void test_spsc_basic(void **state)
{
    (void) state;

    thread_spsc_t *q = thread_spsc_create(8);
    assert_non_null(q);

    bool was_empty = false;
    assert_true(thread_spsc_enqueue(q, PTR(10), &was_empty));
    assert_true(was_empty);
    assert_true(thread_spsc_enqueue(q, PTR(20), &was_empty));
    assert_false(was_empty);
    assert_int_equal(thread_spsc_count(q), 2);

    /* SPSC is strictly FIFO. */
    void *item = NULL;
    assert_true(thread_spsc_dequeue(q, &item));
    assert_int_equal(IVAL(item), 10);
    assert_true(thread_spsc_dequeue(q, &item));
    assert_int_equal(IVAL(item), 20);
    assert_false(thread_spsc_dequeue(q, &item));

    thread_spsc_free(q);
}

static void test_spsc_bound_and_drain(void **state)
{
    (void) state;

    thread_spsc_t *q = thread_spsc_create(3);
    assert_non_null(q);

    assert_true(thread_spsc_enqueue(q, PTR(1), NULL));
    assert_true(thread_spsc_enqueue(q, PTR(2), NULL));
    assert_true(thread_spsc_enqueue(q, PTR(3), NULL));
    assert_false(thread_spsc_enqueue(q, PTR(4), NULL)); /* full */

    void  *batch[8];
    const size_t n = thread_spsc_drain(q, batch, 8);
    assert_int_equal(n, 3);
    assert_int_equal(IVAL(batch[0]), 1); /* FIFO */
    assert_int_equal(IVAL(batch[1]), 2);
    assert_int_equal(IVAL(batch[2]), 3);
    assert_int_equal(thread_spsc_count(q), 0);

    thread_spsc_free(q);
}

/* ------------------------------------------------------------------ */
/* Multi-producer stress: every item must arrive exactly once.        */
/* ------------------------------------------------------------------ */

#define STRESS_PRODUCERS  4
#define STRESS_PER_PROD   50000
#define STRESS_TOTAL      (STRESS_PRODUCERS * STRESS_PER_PROD)

typedef struct {
    thread_mpsc_t *q;
    int            base; /* item ids: base+1 .. base+STRESS_PER_PROD */
} producer_arg_t;

static void *stress_producer(void *argp)
{
    const producer_arg_t *arg = argp;

    for (int i = 1; i <= STRESS_PER_PROD; i++) {
        /* Retry on backpressure — the cap is far below the total in flight. */
        while (!thread_mpsc_enqueue(arg->q, PTR(arg->base + i), NULL)) {
            sched_yield();
        }
    }

    return NULL;
}

static void test_mpsc_concurrent(void **state)
{
    (void) state;

    thread_mpsc_t *q = thread_mpsc_create(1024);
    assert_non_null(q);

    /* seen[id] counts how many times item `id` was dequeued. */
    unsigned char *seen = calloc(STRESS_TOTAL + 1, 1);
    assert_non_null(seen);

    pthread_t      threads[STRESS_PRODUCERS];
    producer_arg_t args[STRESS_PRODUCERS];
    for (int p = 0; p < STRESS_PRODUCERS; p++) {
        args[p].q    = q;
        args[p].base = p * STRESS_PER_PROD;
        assert_int_equal(pthread_create(&threads[p], NULL, stress_producer, &args[p]), 0);
    }

    /* Consumer drains concurrently until every item has been received. */
    void  *batch[128];
    size_t received = 0;
    while (received < STRESS_TOTAL) {
        const size_t n = thread_mpsc_drain(q, batch, 128);
        for (size_t i = 0; i < n; i++) {
            const int id = IVAL(batch[i]);
            assert_in_range(id, 1, STRESS_TOTAL);
            seen[id]++;
            assert_int_equal(seen[id], 1); /* no duplicates */
        }
        received += n;

        if (n == 0) {
            sched_yield();
        }
    }

    for (int p = 0; p < STRESS_PRODUCERS; p++) {
        pthread_join(threads[p], NULL);
    }

    /* Every id seen exactly once; queue fully drained. */
    for (int id = 1; id <= STRESS_TOTAL; id++) {
        assert_int_equal(seen[id], 1);
    }
    assert_int_equal(thread_mpsc_count(q), 0);

    free(seen);
    thread_mpsc_free(q);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_mpsc_basic),
        cmocka_unit_test(test_mpsc_bound),
        cmocka_unit_test(test_mpsc_drain),
        cmocka_unit_test(test_spsc_basic),
        cmocka_unit_test(test_spsc_bound_and_drain),
        cmocka_unit_test(test_mpsc_concurrent),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
