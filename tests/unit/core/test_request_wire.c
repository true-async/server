/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

/* Unit tests for the flat request_wire marshalling type (issue #80, D2). Pure
 * malloc-domain, no PHP/Zend runtime. Covers builders/accessors, non-NUL spans,
 * empty/zero-length fields, routing round-trip, and — the important one — arena
 * growth across realloc, which would corrupt spans if offsets were stored as
 * raw pointers. */

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "core/request_wire.h"

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

    request_wire_t *rw = request_wire_create(7, 42, PTR(0xABCD));
    assert_non_null(rw);

    size_t len = 123;
    assert_null(request_wire_method(rw, &len));
    assert_int_equal(len, 0);

    len = 123;
    assert_null(request_wire_path(rw, &len));
    assert_int_equal(len, 0);

    len = 123;
    assert_null(request_wire_body(rw, &len));
    assert_int_equal(len, 0);

    assert_int_equal(request_wire_header_count(rw), 0);
    assert_false(request_wire_body_complete(rw));

    assert_int_equal(request_wire_reactor_id(rw), 7);
    assert_int_equal(request_wire_stream_id(rw), 42);
    assert_ptr_equal(request_wire_conn(rw), PTR(0xABCD));

    request_wire_free(rw);
}

static void test_method_path_body(void **state)
{
    (void) state;

    request_wire_t *rw = request_wire_create(0, 0, NULL);
    assert_non_null(rw);

    assert_true(request_wire_set_method(rw, "POST", 4));
    assert_true(request_wire_set_path(rw, "/api/users", 10));
    assert_true(request_wire_set_body(rw, "hello", 5, true));

    size_t len;
    ASSERT_SPAN(request_wire_method(rw, &len), len, "POST");
    ASSERT_SPAN(request_wire_path(rw, &len), len, "/api/users");
    ASSERT_SPAN(request_wire_body(rw, &len), len, "hello");
    assert_true(request_wire_body_complete(rw));

    request_wire_free(rw);
}

static void test_non_nul_terminated_span(void **state)
{
    (void) state;

    /* Span carved out of a larger buffer with no NUL — only [off,len) is ours. */
    const char buf[] = "GETPUTPATCH";
    request_wire_t *rw = request_wire_create(0, 0, NULL);

    assert_true(request_wire_set_method(rw, buf + 3, 3)); /* "PUT" */

    size_t len;
    ASSERT_SPAN(request_wire_method(rw, &len), len, "PUT");

    request_wire_free(rw);
}

static void test_headers(void **state)
{
    (void) state;

    request_wire_t *rw = request_wire_create(0, 0, NULL);

    assert_true(request_wire_add_header(rw, "content-type", 12, "application/json", 16));
    assert_true(request_wire_add_header(rw, "accept", 6, "*/*", 3));
    assert_int_equal(request_wire_header_count(rw), 2);

    const char *np, *vp;
    size_t nl, vl;

    assert_true(request_wire_header_at(rw, 0, &np, &nl, &vp, &vl));
    assert_span(np, nl, "content-type");
    assert_span(vp, vl, "application/json");

    assert_true(request_wire_header_at(rw, 1, &np, &nl, &vp, &vl));
    assert_span(np, nl, "accept");
    assert_span(vp, vl, "*/*");

    assert_false(request_wire_header_at(rw, 2, &np, &nl, &vp, &vl));

    request_wire_free(rw);
}

static void test_empty_body_complete(void **state)
{
    (void) state;

    /* GET-style: body set, zero length, complete. */
    request_wire_t *rw = request_wire_create(0, 0, NULL);

    assert_true(request_wire_set_method(rw, "GET", 3));
    assert_true(request_wire_set_body(rw, NULL, 0, true));

    size_t len = 99;
    request_wire_body(rw, &len);
    assert_int_equal(len, 0);
    assert_true(request_wire_body_complete(rw));

    request_wire_free(rw);
}

static void test_streaming_body_flag(void **state)
{
    (void) state;

    request_wire_t *rw = request_wire_create(0, 0, NULL);
    assert_true(request_wire_set_body(rw, "first-chunk", 11, false));

    size_t len;
    ASSERT_SPAN(request_wire_body(rw, &len), len, "first-chunk");
    assert_false(request_wire_body_complete(rw)); /* more streamed later */

    request_wire_free(rw);
}

static void test_method_replaced(void **state)
{
    (void) state;

    request_wire_t *rw = request_wire_create(0, 0, NULL);
    assert_true(request_wire_set_method(rw, "GET", 3));
    assert_true(request_wire_set_method(rw, "DELETE", 6)); /* latest wins */

    size_t len;
    ASSERT_SPAN(request_wire_method(rw, &len), len, "DELETE");

    request_wire_free(rw);
}

/* The important one: force many arena reallocs, then verify every earlier span
 * still reads correctly — offsets must survive realloc (raw pointers would not). */
static void test_arena_growth_keeps_spans(void **state)
{
    (void) state;

    request_wire_t *rw = request_wire_create(1, 2, NULL);

    assert_true(request_wire_set_method(rw, "REPORT", 6));
    assert_true(request_wire_set_path(rw, "/first", 6));

    char name[32], value[512];
    const int count = 200;

    for (int i = 0; i < count; i++) {
        snprintf(name, sizeof(name), "x-header-%d", i);
        memset(value, 'a' + (i % 26), sizeof(value));
        assert_true(request_wire_add_header(rw, name, strlen(name), value, sizeof(value)));
    }

    assert_int_equal(request_wire_header_count(rw), (size_t) count);

    /* Method/path set before all the growth must still be intact. */
    size_t len;
    ASSERT_SPAN(request_wire_method(rw, &len), len, "REPORT");
    ASSERT_SPAN(request_wire_path(rw, &len), len, "/first");

    /* Spot-check headers across the range. */
    for (int i = 0; i < count; i += 37) {
        snprintf(name, sizeof(name), "x-header-%d", i);

        const char *np, *vp;
        size_t nl, vl;
        assert_true(request_wire_header_at(rw, (size_t) i, &np, &nl, &vp, &vl));
        assert_int_equal(nl, strlen(name));
        assert_memory_equal(np, name, nl);
        assert_int_equal(vl, sizeof(value));
        assert_int_equal(vp[0], 'a' + (i % 26));
        assert_int_equal(vp[vl - 1], 'a' + (i % 26));
    }

    request_wire_free(rw);
}

static void test_free_null(void **state)
{
    (void) state;
    request_wire_free(NULL); /* no-op, must not crash */
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_empty_wire),
        cmocka_unit_test(test_method_path_body),
        cmocka_unit_test(test_non_nul_terminated_span),
        cmocka_unit_test(test_headers),
        cmocka_unit_test(test_empty_body_complete),
        cmocka_unit_test(test_streaming_body_flag),
        cmocka_unit_test(test_method_replaced),
        cmocka_unit_test(test_arena_growth_keeps_spans),
        cmocka_unit_test(test_free_null),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
