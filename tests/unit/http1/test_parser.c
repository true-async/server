/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>
#include <string.h>

#include "common/php_sapi_test.h"
#include "http1/http_parser.h"

/*
 * Test: Simple GET request parsing
 */
static void test_parse_simple_get(void **state) {
    (void) state; /* unused */

    const char *request_data =
        "GET /index.html HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "\r\n";

    http1_parser_t *ctx = http_parser_create(10 * 1024 * 1024);
    assert_non_null(ctx);

    int result = http_parser_execute(ctx, request_data, strlen(request_data), NULL);
    assert_int_equal(result, 0); /* 0 = success */
    assert_true(http_parser_is_complete(ctx));

    http_request_t *req = http_parser_get_request(ctx);
    assert_non_null(req);
    assert_true(req->complete);

    /* Check request line */
    assert_non_null(req->method);
    assert_string_equal(ZSTR_VAL(req->method), "GET");

    assert_non_null(req->uri);
    assert_string_equal(ZSTR_VAL(req->uri), "/index.html");

    assert_int_equal(req->http_major, 1);
    assert_int_equal(req->http_minor, 1);

    /* Check headers (note: header names are lowercase) */
    assert_non_null(req->headers);
    assert_int_equal(zend_hash_num_elements(req->headers), 1);

    zval *host_header = zend_hash_str_find(req->headers, "host", sizeof("host") - 1);
    assert_non_null(host_header);
    assert_int_equal(Z_TYPE_P(host_header), IS_STRING);
    assert_string_equal(Z_STRVAL_P(host_header), "localhost");

    /* Check body */
    assert_null(req->body);

    /* Keep-alive should be true for HTTP/1.1 */
    assert_true(req->keep_alive);

    http_parser_destroy(ctx);
}

/*
 * Test: POST request with body
 */
static void test_parse_post_with_body(void **state) {
    (void) state; /* unused */

    const char *request_data =
        "POST /api/data HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: 13\r\n"
        "\r\n"
        "{\"key\":\"val\"}";

    http1_parser_t *ctx = http_parser_create(10 * 1024 * 1024);
    assert_non_null(ctx);

    int result = http_parser_execute(ctx, request_data, strlen(request_data), NULL);
    assert_int_equal(result, 0); /* 0 = success */
    assert_true(http_parser_is_complete(ctx));

    http_request_t *req = http_parser_get_request(ctx);
    assert_non_null(req);

    /* Check request line */
    assert_non_null(req->method);
    assert_string_equal(ZSTR_VAL(req->method), "POST");

    assert_non_null(req->uri);
    assert_string_equal(ZSTR_VAL(req->uri), "/api/data");

    /* Check headers */
    assert_non_null(req->headers);
    assert_int_equal(zend_hash_num_elements(req->headers), 3);

    /* Check body */
    assert_non_null(req->body);
    assert_string_equal(ZSTR_VAL(req->body), "{\"key\":\"val\"}");
    assert_int_equal(req->content_length, 13);

    http_parser_destroy(ctx);
}

/*
 * Test: Multiple HTTP methods
 */
static void test_parse_various_methods(void **state) {
    (void) state; /* unused */

    const char *methods[] = {"GET", "POST", "PUT", "DELETE", "HEAD", "OPTIONS", "PATCH"};
    int method_count = sizeof(methods) / sizeof(methods[0]);

    for (int i = 0; i < method_count; i++) {
        char request_data[256];
        snprintf(request_data, sizeof(request_data),
                 "%s /test HTTP/1.1\r\nHost: localhost\r\n\r\n",
                 methods[i]);

        http1_parser_t *ctx = http_parser_create(10 * 1024 * 1024);
        assert_non_null(ctx);

        int result = http_parser_execute(ctx, request_data, strlen(request_data), NULL);
        assert_int_equal(result, 0); /* 0 = success */
        assert_true(http_parser_is_complete(ctx));

        http_request_t *req = http_parser_get_request(ctx);
        assert_non_null(req);
        assert_non_null(req->method);
        assert_string_equal(ZSTR_VAL(req->method), methods[i]);

        http_parser_destroy(ctx);
    }
}

/*
 * Test: URI with query string
 */
static void test_parse_uri_with_query(void **state) {
    (void) state; /* unused */

    const char *request_data =
        "GET /search?q=test&page=1 HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "\r\n";

    http1_parser_t *ctx = http_parser_create(10 * 1024 * 1024);
    assert_non_null(ctx);

    int result = http_parser_execute(ctx, request_data, strlen(request_data), NULL);
    assert_int_equal(result, 0); /* 0 = success */

    http_request_t *req = http_parser_get_request(ctx);
    assert_non_null(req);
    assert_non_null(req->uri);
    assert_string_equal(ZSTR_VAL(req->uri), "/search?q=test&page=1");

    http_parser_destroy(ctx);
}

/*
 * Test: Keep-Alive detection
 */
static void test_keep_alive_detection(void **state) {
    (void) state; /* unused */

    /* HTTP/1.1 without Connection header - should be keep-alive */
    {
        const char *request_data =
            "GET / HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "\r\n";

        http1_parser_t *ctx = http_parser_create(10 * 1024 * 1024);
        http_parser_execute(ctx, request_data, strlen(request_data), NULL);

        http_request_t *req = http_parser_get_request(ctx);
        assert_true(req->keep_alive);

        http_parser_destroy(ctx);
    }

    /* HTTP/1.1 with Connection: close - should NOT be keep-alive */
    {
        const char *request_data =
            "GET / HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "Connection: close\r\n"
            "\r\n";

        http1_parser_t *ctx = http_parser_create(10 * 1024 * 1024);
        http_parser_execute(ctx, request_data, strlen(request_data), NULL);

        http_request_t *req = http_parser_get_request(ctx);
        assert_false(req->keep_alive);

        http_parser_destroy(ctx);
    }

    /* HTTP/1.0 without Connection header - should NOT be keep-alive */
    {
        const char *request_data =
            "GET / HTTP/1.0\r\n"
            "Host: localhost\r\n"
            "\r\n";

        http1_parser_t *ctx = http_parser_create(10 * 1024 * 1024);
        http_parser_execute(ctx, request_data, strlen(request_data), NULL);

        http_request_t *req = http_parser_get_request(ctx);
        assert_false(req->keep_alive);

        http_parser_destroy(ctx);
    }

    /* HTTP/1.0 with Connection: keep-alive - should be keep-alive */
    {
        const char *request_data =
            "GET / HTTP/1.0\r\n"
            "Host: localhost\r\n"
            "Connection: keep-alive\r\n"
            "\r\n";

        http1_parser_t *ctx = http_parser_create(10 * 1024 * 1024);
        http_parser_execute(ctx, request_data, strlen(request_data), NULL);

        http_request_t *req = http_parser_get_request(ctx);
        assert_true(req->keep_alive);

        http_parser_destroy(ctx);
    }
}

/*
 * Test: Invalid request handling
 */
static void test_parse_invalid_request(void **state) {
    (void) state; /* unused */

    /* Malformed request line */
    {
        const char *request_data = "INVALID REQUEST\r\n\r\n";

        http1_parser_t *ctx = http_parser_create(10 * 1024 * 1024);
        int result = http_parser_execute(ctx, request_data, strlen(request_data), NULL);

        /* llhttp should reject malformed request */
        assert_true(result < 0);

        http_parser_destroy(ctx);
    }

    /* Empty request */
    {
        const char *request_data = "";

        http1_parser_t *ctx = http_parser_create(10 * 1024 * 1024);
        int result = http_parser_execute(ctx, request_data, strlen(request_data), NULL);
        (void)result; /* May be 0 for empty request */

        assert_false(http_parser_is_complete(ctx));

        http_parser_destroy(ctx);
    }
}

/*
 * Test: Multiple headers with same name (case-insensitive)
 */
static void test_parse_multiple_headers(void **state) {
    (void) state; /* unused */

    const char *request_data =
        "GET / HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Accept: text/html\r\n"
        "Accept-Encoding: gzip\r\n"
        "User-Agent: TestClient/1.0\r\n"
        "\r\n";

    http1_parser_t *ctx = http_parser_create(10 * 1024 * 1024);
    assert_non_null(ctx);

    int result = http_parser_execute(ctx, request_data, strlen(request_data), NULL);
    assert_int_equal(result, 0); /* 0 = success */

    http_request_t *req = http_parser_get_request(ctx);
    assert_non_null(req);
    assert_non_null(req->headers);
    assert_int_equal(zend_hash_num_elements(req->headers), 4);

    /* Verify all headers are present (note: header names are lowercase) */
    zval *header;

    header = zend_hash_str_find(req->headers, "host", sizeof("host") - 1);
    assert_non_null(header);
    assert_string_equal(Z_STRVAL_P(header), "localhost");

    header = zend_hash_str_find(req->headers, "accept", sizeof("accept") - 1);
    assert_non_null(header);
    assert_string_equal(Z_STRVAL_P(header), "text/html");

    header = zend_hash_str_find(req->headers, "accept-encoding", sizeof("accept-encoding") - 1);
    assert_non_null(header);
    assert_string_equal(Z_STRVAL_P(header), "gzip");

    header = zend_hash_str_find(req->headers, "user-agent", sizeof("user-agent") - 1);
    assert_non_null(header);
    assert_string_equal(Z_STRVAL_P(header), "TestClient/1.0");

    http_parser_destroy(ctx);
}

/*
 * Test: Parser reset for connection reuse
 */
static void test_parser_reset(void **state) {
    (void) state; /* unused */

    const char *request1 =
        "GET /first HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "\r\n";

    const char *request2 =
        "POST /second HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Content-Length: 4\r\n"
        "\r\n"
        "test";

    http1_parser_t *ctx = http_parser_create(10 * 1024 * 1024);
    assert_non_null(ctx);

    /* Parse first request */
    http_parser_execute(ctx, request1, strlen(request1), NULL);
    assert_true(http_parser_is_complete(ctx));

    http_request_t *req1 = http_parser_get_request(ctx);
    assert_string_equal(ZSTR_VAL(req1->method), "GET");
    assert_string_equal(ZSTR_VAL(req1->uri), "/first");

    /* Reset parser */
    http_parser_reset(ctx);
    assert_false(http_parser_is_complete(ctx));

    /* Parse second request */
    http_parser_execute(ctx, request2, strlen(request2), NULL);
    assert_true(http_parser_is_complete(ctx));

    http_request_t *req2 = http_parser_get_request(ctx);
    assert_string_equal(ZSTR_VAL(req2->method), "POST");
    assert_string_equal(ZSTR_VAL(req2->uri), "/second");
    assert_non_null(req2->body);
    assert_string_equal(ZSTR_VAL(req2->body), "test");

    http_parser_destroy(ctx);
}

/*
 * Setup: Initialize PHP runtime
 */
static int group_setup(void **state) {
    (void) state; /* unused */
    return php_test_runtime_init();
}

/*
 * Teardown: Shutdown PHP runtime
 */
static int group_teardown(void **state) {
    (void) state; /* unused */
    php_test_runtime_shutdown();
    return 0;
}

/*
 * Main test suite
 */
int main(void) {
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_parse_simple_get),
        cmocka_unit_test(test_parse_post_with_body),
        cmocka_unit_test(test_parse_various_methods),
        cmocka_unit_test(test_parse_uri_with_query),
        cmocka_unit_test(test_keep_alive_detection),
        cmocka_unit_test(test_parse_invalid_request),
        cmocka_unit_test(test_parse_multiple_headers),
        cmocka_unit_test(test_parser_reset),
    };

    return cmocka_run_group_tests(tests, group_setup, group_teardown);
}
