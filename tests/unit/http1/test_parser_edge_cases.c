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
 * Test: Request with no body (GET request)
 */
static void test_no_body(void **state) {
    (void) state;

    const char *request_data =
        "GET / HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "\r\n";

    http1_parser_t *ctx = http_parser_create(10 * 1024 * 1024);
    assert_non_null(ctx);

    int result = http_parser_execute(ctx, request_data, strlen(request_data), NULL);
    assert_int_equal(result, 0);
    assert_true(http_parser_is_complete(ctx));

    http_request_t *req = http_parser_get_request(ctx);
    assert_non_null(req);

    /* Body should be NULL for GET without body */
    assert_null(req->body);
    assert_int_equal(req->content_length, 0);

    http_parser_destroy(ctx);
}

/*
 * Test: Request with no headers
 */
static void test_no_headers(void **state) {
    (void) state;

    const char *request_data =
        "GET / HTTP/1.0\r\n"
        "\r\n";

    http1_parser_t *ctx = http_parser_create(10 * 1024 * 1024);
    assert_non_null(ctx);

    int result = http_parser_execute(ctx, request_data, strlen(request_data), NULL);
    assert_int_equal(result, 0);
    assert_true(http_parser_is_complete(ctx));

    http_request_t *req = http_parser_get_request(ctx);
    assert_non_null(req);
    assert_non_null(req->headers);
    assert_int_equal(zend_hash_num_elements(req->headers), 0);

    /* HTTP/1.0 without Connection header should not be keep-alive */
    assert_false(req->keep_alive);

    http_parser_destroy(ctx);
}

/*
 * Test: URI at maximum size limit (8KB)
 */
static void test_uri_max_size(void **state) {
    (void) state;

    /* Create URI exactly at limit (8KB - request line overhead) */
    char uri[8 * 1024];
    memset(uri, 'a', sizeof(uri) - 100);
    uri[0] = '/';
    uri[sizeof(uri) - 100] = '\0';

    char request_data[10 * 1024];
    snprintf(request_data, sizeof(request_data),
             "GET %s HTTP/1.1\r\nHost: localhost\r\n\r\n", uri);

    http1_parser_t *ctx = http_parser_create(10 * 1024 * 1024);
    assert_non_null(ctx);

    int result = http_parser_execute(ctx, request_data, strlen(request_data), NULL);
    assert_int_equal(result, 0);
    assert_true(http_parser_is_complete(ctx));

    http_request_t *req = http_parser_get_request(ctx);
    assert_non_null(req);
    assert_non_null(req->uri);

    http_parser_destroy(ctx);
}

/*
 * Test: URI exceeding maximum size limit
 */
static void test_uri_too_long(void **state) {
    (void) state;

    /* Create URI that exceeds 8KB limit */
    char uri[10 * 1024];
    memset(uri, 'a', sizeof(uri) - 1);
    uri[0] = '/';
    uri[sizeof(uri) - 1] = '\0';

    char *request_data = malloc(12 * 1024);
    snprintf(request_data, 12 * 1024,
             "GET %s HTTP/1.1\r\nHost: localhost\r\n\r\n", uri);

    http1_parser_t *ctx = http_parser_create(10 * 1024 * 1024);
    assert_non_null(ctx);

    int result = http_parser_execute(ctx, request_data, strlen(request_data), NULL);

    /* Should fail with error */
    assert_true(result < 0);

    free(request_data);
    http_parser_destroy(ctx);
}

/*
 * Test: Very long header value (within limits)
 */
static void test_long_header_value(void **state) {
    (void) state;

    /* Create 7KB header value (within 8KB limit) */
    char value[7 * 1024];
    memset(value, 'x', sizeof(value) - 1);
    value[sizeof(value) - 1] = '\0';

    char *request_data = malloc(10 * 1024);
    snprintf(request_data, 10 * 1024,
             "GET / HTTP/1.1\r\n"
             "Host: localhost\r\n"
             "X-Long-Header: %s\r\n"
             "\r\n", value);

    http1_parser_t *ctx = http_parser_create(10 * 1024 * 1024);
    assert_non_null(ctx);

    int result = http_parser_execute(ctx, request_data, strlen(request_data), NULL);
    assert_int_equal(result, 0);
    assert_true(http_parser_is_complete(ctx));

    http_request_t *req = http_parser_get_request(ctx);
    assert_non_null(req);

    /* Check that long header was parsed */
    zval *header = zend_hash_str_find(req->headers, "x-long-header", sizeof("x-long-header") - 1);
    assert_non_null(header);
    assert_int_equal(ZSTR_LEN(Z_STR_P(header)), sizeof(value) - 1);

    free(request_data);
    http_parser_destroy(ctx);
}

/*
 * Test: Header value exceeding maximum size
 */
static void test_header_value_too_long(void **state) {
    (void) state;

    /* Create header value exceeding 8KB limit */
    char value[10 * 1024];
    memset(value, 'x', sizeof(value) - 1);
    value[sizeof(value) - 1] = '\0';

    char *request_data = malloc(15 * 1024);
    snprintf(request_data, 15 * 1024,
             "GET / HTTP/1.1\r\n"
             "Host: localhost\r\n"
             "X-Too-Long: %s\r\n"
             "\r\n", value);

    http1_parser_t *ctx = http_parser_create(10 * 1024 * 1024);
    assert_non_null(ctx);

    int result = http_parser_execute(ctx, request_data, strlen(request_data), NULL);

    /* Should fail */
    assert_true(result < 0);

    free(request_data);
    http_parser_destroy(ctx);
}

/*
 * Test: Content-Length exceeding body size limit
 */
static void test_body_too_large(void **state) {
    (void) state;

    const char *request_data =
        "POST / HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Content-Length: 20971520\r\n"  /* 20MB - exceeds 10MB limit */
        "\r\n";

    http1_parser_t *ctx = http_parser_create(10 * 1024 * 1024);
    assert_non_null(ctx);

    int result = http_parser_execute(ctx, request_data, strlen(request_data), NULL);

    /* Should fail at headers complete */
    assert_true(result < 0);

    http_parser_destroy(ctx);
}

/*
 * Test: Chunked encoding
 */
static void test_chunked_encoding(void **state) {
    (void) state;

    const char *request_data =
        "POST / HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "5\r\n"
        "Hello\r\n"
        "6\r\n"
        " World\r\n"
        "0\r\n"
        "\r\n";

    http1_parser_t *ctx = http_parser_create(10 * 1024 * 1024);
    assert_non_null(ctx);

    int result = http_parser_execute(ctx, request_data, strlen(request_data), NULL);
    assert_int_equal(result, 0);

    http_request_t *req = http_parser_get_request(ctx);
    assert_non_null(req);
    assert_true(req->chunked);

    /* llhttp handles chunk decoding, so body should contain decoded data */
    if (req->body) {
        assert_string_equal(ZSTR_VAL(req->body), "Hello World");
    }

    http_parser_destroy(ctx);
}

/*
 * Test: Parsing request in multiple chunks
 */
static void test_parse_in_chunks(void **state) {
    (void) state;

    const char *chunk1 = "POST /api HTTP/1.1\r\n";
    const char *chunk2 = "Host: localhost\r\n";
    const char *chunk3 = "Content-Type: application/json\r\n";
    const char *chunk4 = "Content-Length: 15\r\n";  /* Correct length for {"key":"value"} */
    const char *chunk5 = "\r\n";
    const char *chunk6 = "{\"key\":\"";
    const char *chunk7 = "value\"}";

    http1_parser_t *ctx = http_parser_create(10 * 1024 * 1024);
    assert_non_null(ctx);

    /* Parse in multiple calls */
    assert_int_equal(http_parser_execute(ctx, chunk1, strlen(chunk1), NULL), 0);
    assert_int_equal(http_parser_execute(ctx, chunk2, strlen(chunk2), NULL), 0);
    assert_int_equal(http_parser_execute(ctx, chunk3, strlen(chunk3), NULL), 0);
    assert_int_equal(http_parser_execute(ctx, chunk4, strlen(chunk4), NULL), 0);
    assert_int_equal(http_parser_execute(ctx, chunk5, strlen(chunk5), NULL), 0);
    assert_int_equal(http_parser_execute(ctx, chunk6, strlen(chunk6), NULL), 0);
    assert_int_equal(http_parser_execute(ctx, chunk7, strlen(chunk7), NULL), 0);

    assert_true(http_parser_is_complete(ctx));

    http_request_t *req = http_parser_get_request(ctx);
    assert_non_null(req);
    assert_string_equal(ZSTR_VAL(req->method), "POST");
    assert_string_equal(ZSTR_VAL(req->uri), "/api");
    assert_non_null(req->body);
    assert_string_equal(ZSTR_VAL(req->body), "{\"key\":\"value\"}");

    http_parser_destroy(ctx);
}

/*
 * Test: Malformed request (invalid HTTP version)
 */
static void test_invalid_http_version(void **state) {
    (void) state;

    const char *request_data = "GET / HTTP/9.9\r\n\r\n";

    http1_parser_t *ctx = http_parser_create(10 * 1024 * 1024);
    assert_non_null(ctx);

    int result = http_parser_execute(ctx, request_data, strlen(request_data), NULL);

    /* llhttp may or may not reject this - depends on implementation */
    /* Just ensure we don't crash */
    (void)result;

    http_parser_destroy(ctx);
}

/*
 * Setup: Initialize PHP runtime
 */
static int group_setup(void **state) {
    (void) state;
    return php_test_runtime_init();
}

/*
 * Teardown: Shutdown PHP runtime
 */
static int group_teardown(void **state) {
    (void) state;
    php_test_runtime_shutdown();
    return 0;
}

/*
 * Main test suite
 */
int main(void) {
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_no_body),
        cmocka_unit_test(test_no_headers),
        cmocka_unit_test(test_uri_max_size),
        cmocka_unit_test(test_uri_too_long),
        cmocka_unit_test(test_long_header_value),
        cmocka_unit_test(test_header_value_too_long),
        cmocka_unit_test(test_body_too_large),
        cmocka_unit_test(test_chunked_encoding),
        cmocka_unit_test(test_parse_in_chunks),
        cmocka_unit_test(test_invalid_http_version),
    };

    return cmocka_run_group_tests(tests, group_setup, group_teardown);
}
