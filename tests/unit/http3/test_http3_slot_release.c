/* Unit tests for the slot-release route of an HTTP/3 stream — where a slot goes
 * home when the request's last reference drops on a worker thread. No server,
 * no socket, no reactor: the rule is a predicate over the stream, and these
 * cases are the four shapes it must answer for.
 *
 * The case that matters is the abandoned request: the connection is gone and a
 * worker still holds the request. Reading the route through the connection —
 * what the code did before #261 — answers "free it here" for exactly that
 * population, and here it is caught by a stream with a NULL conn. */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <string.h>
#include <cmocka.h>

#include <php.h>
#include "http3/http3_stream.h"
#include "http3/http3_stream_pool.h"

/* The predicate reads three fields; the rest of the slot is undefined storage
 * in the pool too, so a zeroed stack stream is the honest fixture. */
static void slot_init(http3_stream_t *s, http3_stream_pool_t *pool,
                      const bool reactor_owned)
{
    memset(s, 0, sizeof(*s));
    s->pool = pool;
    s->reactor_owned = reactor_owned;
}

/* The abandoned request: teardown has NULLed conn, the worker holds the last
 * reference, and the slab is still the reactor's. */
static void test_abandoned_request_goes_to_the_reactor(void **state)
{
    (void)state;
    http3_stream_pool_t pool = {0};
    http3_stream_t s;

    slot_init(&s, &pool, true);
    s.conn = NULL;

    assert_true(http3_stream_slot_goes_to_reactor(&s));
}

static void test_live_connection_goes_to_the_reactor(void **state)
{
    (void)state;
    http3_stream_pool_t pool = {0};
    http3_stream_t s;

    slot_init(&s, &pool, true);
    s.conn = (struct http3_connection_s *)(uintptr_t)0xC0FFEE;

    assert_true(http3_stream_slot_goes_to_reactor(&s));
}

/* Shutdown: the listener closed the pool, so no reactor takes work for it and
 * the posts would refuse. The worker reclaims the slot itself. */
static void test_closed_pool_stays_local(void **state)
{
    (void)state;
    http3_stream_pool_t pool = {0};
    http3_stream_t s;

    pool.closed = true;
    slot_init(&s, &pool, true);

    assert_false(http3_stream_slot_goes_to_reactor(&s));
}

/* Single-thread mode: the slab belongs to this thread. */
static void test_single_thread_stays_local(void **state)
{
    (void)state;
    http3_stream_pool_t pool = {0};
    http3_stream_t s;

    slot_init(&s, &pool, false);

    assert_false(http3_stream_slot_goes_to_reactor(&s));
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_abandoned_request_goes_to_the_reactor),
        cmocka_unit_test(test_live_connection_goes_to_the_reactor),
        cmocka_unit_test(test_closed_pool_stays_local),
        cmocka_unit_test(test_single_thread_stays_local),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
