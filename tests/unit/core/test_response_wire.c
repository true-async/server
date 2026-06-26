/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

/* Unit tests for the flat response_wire marshalling type (issue #80, D3), the
 * return-path mirror of request_wire. Pure malloc-domain, no PHP/Zend runtime.
 * Covers status, builders/accessors, non-NUL spans, empty/zero-length body,
 * routing round-trip, and arena growth across realloc (offsets, not pointers). */

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "core/response_wire.h"

#define PTR(i) ((void *) (intptr_t) (i))

/* Evaluate an accessor that writes *len BEFORE reading len — the accessor and
 * the `len` argument are unsequenced inside a single assert_span(...) call. */
#define ASSERT_SPAN(accessor_call, len_var, expect) \
    do { const char *sp__ = (accessor_call); assert_span(sp__, (len_var), (expect)); } while (0)

/* assert a returned span equals an expected C-string (by length + bytes) */
static void assert_span(const char *ptr, size_t len, const char *expect)
{
    const size_t elen = strlen(expect);
    assert_int_equal(len, elen);
    if (elen != 0) {
        assert_non_null(ptr);
        assert_memory_equal(ptr, expect, elen);
    }
}

static void test_empty_wire(void **state)
{
    (void) state;

    response_wire_t *rw = response_wire_create(7, 42, PTR(0xABCD));
    assert_non_null(rw);

    assert_int_equal(response_wire_status(rw), 0);

    size_t len = 123;
    assert_null(response_wire_body(rw, &len));
    assert_int_equal(len, 0);

    assert_int_equal(response_wire_header_count(rw), 0);
    assert_false(response_wire_body_complete(rw));

    assert_int_equal(response_wire_reactor_id(rw), 7);
    assert_int_equal(response_wire_stream_id(rw), 42);
    assert_ptr_equal(response_wire_conn(rw), PTR(0xABCD));

    response_wire_free(rw);
}

static void test_status_headers_body(void **state)
{
    (void) state;

    response_wire_t *rw = response_wire_create(0, 0, NULL);
    assert_non_null(rw);

    response_wire_set_status(rw, 200);
    assert_true(response_wire_add_header(rw, "content-type", 12, "text/plain", 10));
    assert_true(response_wire_set_body(rw, "hello", 5, true));

    assert_int_equal(response_wire_status(rw), 200);

    size_t len;
    ASSERT_SPAN(response_wire_body(rw, &len), len, "hello");
    assert_true(response_wire_body_complete(rw));

    const char *np, *vp;
    size_t nl, vl;
    assert_true(response_wire_header_at(rw, 0, &np, &nl, &vp, &vl));
    assert_span(np, nl, "content-type");
    assert_span(vp, vl, "text/plain");

    response_wire_free(rw);
}

static void test_status_replaced(void **state)
{
    (void) state;

    response_wire_t *rw = response_wire_create(0, 0, NULL);
    response_wire_set_status(rw, 200);
    response_wire_set_status(rw, 404); /* latest wins */
    assert_int_equal(response_wire_status(rw), 404);

    response_wire_free(rw);
}

static void test_non_nul_terminated_span(void **state)
{
    (void) state;

    /* Header value carved out of a larger buffer with no NUL. */
    const char buf[] = "gzip,deflate,br";
    response_wire_t *rw = response_wire_create(0, 0, NULL);

    assert_true(response_wire_add_header(rw, "content-encoding", 16, buf + 5, 7)); /* "deflate" */

    const char *np, *vp;
    size_t nl, vl;
    assert_true(response_wire_header_at(rw, 0, &np, &nl, &vp, &vl));
    assert_span(np, nl, "content-encoding");
    assert_span(vp, vl, "deflate");

    response_wire_free(rw);
}

static void test_headers(void **state)
{
    (void) state;

    response_wire_t *rw = response_wire_create(0, 0, NULL);

    assert_true(response_wire_add_header(rw, "content-type", 12, "application/json", 16));
    assert_true(response_wire_add_header(rw, "cache-control", 13, "no-store", 8));
    assert_int_equal(response_wire_header_count(rw), 2);

    const char *np, *vp;
    size_t nl, vl;

    assert_true(response_wire_header_at(rw, 0, &np, &nl, &vp, &vl));
    assert_span(np, nl, "content-type");
    assert_span(vp, vl, "application/json");

    assert_true(response_wire_header_at(rw, 1, &np, &nl, &vp, &vl));
    assert_span(np, nl, "cache-control");
    assert_span(vp, vl, "no-store");

    assert_false(response_wire_header_at(rw, 2, &np, &nl, &vp, &vl));

    response_wire_free(rw);
}

static void test_empty_body_complete(void **state)
{
    (void) state;

    /* 204-style: body set, zero length, complete. */
    response_wire_t *rw = response_wire_create(0, 0, NULL);

    response_wire_set_status(rw, 204);
    assert_true(response_wire_set_body(rw, NULL, 0, true));

    size_t len = 99;
    response_wire_body(rw, &len);
    assert_int_equal(len, 0);
    assert_true(response_wire_body_complete(rw));

    response_wire_free(rw);
}

static void test_streaming_body_flag(void **state)
{
    (void) state;

    response_wire_t *rw = response_wire_create(0, 0, NULL);
    assert_true(response_wire_set_body(rw, "first-chunk", 11, false));

    size_t len;
    ASSERT_SPAN(response_wire_body(rw, &len), len, "first-chunk");
    assert_false(response_wire_body_complete(rw)); /* more streamed later */

    response_wire_free(rw);
}

/* Force many arena reallocs, then verify every earlier span still reads
 * correctly — offsets must survive realloc (raw pointers would not). */
static void test_arena_growth_keeps_spans(void **state)
{
    (void) state;

    response_wire_t *rw = response_wire_create(1, 2, NULL);

    response_wire_set_status(rw, 200);

    char name[32], value[512];
    const int count = 200;

    for (int i = 0; i < count; i++) {
        snprintf(name, sizeof(name), "x-header-%d", i);
        memset(value, 'a' + (i % 26), sizeof(value));
        assert_true(response_wire_add_header(rw, name, strlen(name), value, sizeof(value)));
    }

    assert_int_equal(response_wire_header_count(rw), (size_t) count);

    /* Body set last, then re-read — but status set before all the growth must
     * still be intact. */
    assert_true(response_wire_set_body(rw, "tail-body", 9, true));

    assert_int_equal(response_wire_status(rw), 200);

    size_t len;
    ASSERT_SPAN(response_wire_body(rw, &len), len, "tail-body");

    /* Spot-check headers across the range. */
    for (int i = 0; i < count; i += 37) {
        snprintf(name, sizeof(name), "x-header-%d", i);

        const char *np, *vp;
        size_t nl, vl;
        assert_true(response_wire_header_at(rw, (size_t) i, &np, &nl, &vp, &vl));
        assert_int_equal(nl, strlen(name));
        assert_memory_equal(np, name, nl);
        assert_int_equal(vl, sizeof(value));
        assert_int_equal(vp[0], 'a' + (i % 26));
        assert_int_equal(vp[vl - 1], 'a' + (i % 26));
    }

    response_wire_free(rw);
}

static void test_free_null(void **state)
{
    (void) state;
    response_wire_free(NULL); /* no-op, must not crash */
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_empty_wire),
        cmocka_unit_test(test_status_headers_body),
        cmocka_unit_test(test_status_replaced),
        cmocka_unit_test(test_non_nul_terminated_span),
        cmocka_unit_test(test_headers),
        cmocka_unit_test(test_empty_body_complete),
        cmocka_unit_test(test_streaming_body_flag),
        cmocka_unit_test(test_arena_growth_keeps_spans),
        cmocka_unit_test(test_free_null),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
