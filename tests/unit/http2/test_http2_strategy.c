/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

/**
 * @file test_http2_strategy.c
 * @brief Step 1 smoke tests for the HTTP/2 strategy skeleton.
 *
 * Scope is intentionally narrow — we verify that:
 *   - http_protocol_strategy_http2_create() returns a non-NULL strategy
 *     when HAVE_HTTP2 is defined;
 *   - the vtable is fully populated (no NULL function pointers);
 *   - type/name identify HTTP/2 correctly;
 *   - feed() returns the documented "not implemented" error so mis-wired
 *     listeners surface immediately instead of silently dropping bytes;
 *   - destroy/cleanup free without leaking under ASan.
 *
 * Steps 2+ replace the "feed returns -1" assertion with real session
 * behaviour. These smoke tests stay green throughout.
 *
 * Protocol detection (preface + "GET" prefix) is exercised by the
 * connection-layer tests; it already passed before HTTP/2 existed.
 */

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include <stdlib.h>
#include <string.h>

#include "common/php_sapi_test.h"
#include "http_protocol_strategy.h"
#include "http_connection.h"
#include "http1/http_parser.h"

/* Linker stubs for symbols defined in PHP-SAPI-heavy translation units
 * that aren't part of the unit-test link graph. None of these are
 * exercised by the strategy smoke tests — Step 1 only verifies the
 * vtable shape + the "not-really-implemented" feed() path. Step 2+
 * session tests live in their own binary and don't need these stubs. */
http1_parser_t *parser_pool_acquire(void) { return NULL; }
void            parser_pool_return (http1_parser_t *p) { (void)p; }

int        http_response_get_status  (zend_object *o) { (void)o; return 0; }
HashTable *http_response_get_headers (zend_object *o) { (void)o; return NULL; }
HashTable *http_response_get_trailers(zend_object *o) { (void)o; return NULL; }
const char *http_response_get_body   (zend_object *o, size_t *l) {
    (void)o; if (l) *l = 0; return NULL;
}
bool http_response_is_committed(zend_object *o)  { (void)o; return false; }
void http_response_set_committed(zend_object *o) { (void)o; }
void http_response_reset_to_error(zend_object *o, int s, const char *m) {
    (void)o; (void)s; (void)m;
}
void http_response_set_protocol_version(zend_object *o, const char *v) {
    (void)o; (void)v;
}
zend_class_entry *http_response_ce = NULL;
zval *http_request_create_from_parsed(http_request_t *r) { (void)r; return NULL; }
void http_server_on_request_sample(http_server_object *s,
                                   uint64_t sojourn, uint64_t service,
                                   uint64_t now_ns) {
    (void)s; (void)sojourn; (void)service; (void)now_ns;
}

bool http_connection_send(http_connection_t *c, const char *d, size_t l) {
    (void)c; (void)d; (void)l; return false;
}

bool http_connection_send_batched(http_connection_t *c, void *buf, size_t len) {
    (void)c; (void)buf; (void)len; return false;
}

void http_connection_destroy(http_connection_t *c) { (void)c; }

void http_connection_on_request_ready(http_connection_t *c, http_request_t *r) {
    (void)c; (void)r;
}

/* http_server_view_default is the const fallback used when no server is
 * attached to a connection — strategies cache it via http_server_view().
 * Tests don't assert on its fields, so a zero-initialized const is enough
 * to satisfy the linker. */
const http_server_view_t http_server_view_default;

const http_server_view_t *http_server_view(const http_server_object *s) {
    (void)s; return &http_server_view_default;
}

zend_string *http_server_get_alt_svc_value(const http_server_object *s) {
    (void)s; return NULL;
}

void http_response_set_alt_svc_if_unset(zend_object *o, const char *v, size_t l) {
    (void)o; (void)v; (void)l;
}

static void test_strategy_create_vtable_complete(void **state)
{
    (void)state;

    http_protocol_strategy_t *const s = http_protocol_strategy_http2_create();
    assert_non_null(s);

    assert_int_equal((int)s->protocol_type, (int)HTTP_PROTOCOL_HTTP2);
    assert_non_null(s->name);
    assert_string_equal(s->name, "HTTP/2");

    /* Every vtable slot must be filled — NULL would crash the
     * connection layer when it does its unconditional dispatch. */
    assert_non_null(s->feed);
    assert_non_null(s->send_response);
    assert_non_null(s->reset);
    assert_non_null(s->cleanup);

    /* on_request_ready is wired by the connection layer after create,
     * so it's legitimately NULL here. */
    assert_null(s->on_request_ready);

    http_protocol_strategy_destroy(s);
}

static void test_strategy_feed_returns_not_implemented(void **state)
{
    (void)state;

    http_protocol_strategy_t *const s = http_protocol_strategy_http2_create();
    assert_non_null(s);

    /* Passing NULL for conn is safe at Step 1 — feed() doesn't touch it
     * beyond forwarding to the (not-yet-implemented) session. Step 2+
     * will add a real conn fixture here. */
    size_t consumed = 999;   /* poisoned sentinel */
    const int rc = s->feed(s, NULL, "\x00", 1, &consumed);

    assert_int_equal(rc, -1);
    assert_int_equal((int)consumed, 0);

    http_protocol_strategy_destroy(s);
}

static void test_strategy_destroy_null_safe(void **state)
{
    (void)state;
    /* Matches the documented contract of http_protocol_strategy_destroy
     * — silently accepts NULL so the connection layer doesn't need a
     * defensive check in its teardown. */
    http_protocol_strategy_destroy(NULL);
}

static int group_setup(void **state)
{
    (void)state;
    return php_test_runtime_init();
}

static int group_teardown(void **state)
{
    (void)state;
    php_test_runtime_shutdown();
    return 0;
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_strategy_create_vtable_complete),
        cmocka_unit_test(test_strategy_feed_returns_not_implemented),
        cmocka_unit_test(test_strategy_destroy_null_safe),
    };
    return cmocka_run_group_tests(tests, group_setup, group_teardown);
}
