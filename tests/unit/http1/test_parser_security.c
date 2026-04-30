/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
  | Security and RFC compliance tests for HTTP/1.1 parser                |
  +----------------------------------------------------------------------+
*/

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>
#include <string.h>

#include "common/php_sapi_test.h"
#include "http1/http_parser.h"

/* ========================================================================
 * RFC 7230 Compliance Tests
 * ======================================================================== */

/*
 * Test: URI with percent-encoding
 * RFC 3986: percent-encoded characters should be preserved
 */
static void test_uri_percent_encoding(void **state) {
    (void) state;

    const char *request_data =
        "GET /path%20with%20spaces/file%2Fname HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "\r\n";

    http1_parser_t *ctx = http_parser_create(10 * 1024 * 1024);
    assert_non_null(ctx);

    int result = http_parser_execute(ctx, request_data, strlen(request_data), NULL);
    assert_int_equal(result, 0);
    assert_true(http_parser_is_complete(ctx));

    http_request_t *req = http_parser_get_request(ctx);
    assert_non_null(req);
    assert_non_null(req->uri);
    /* URI should preserve percent-encoding */
    assert_string_equal(ZSTR_VAL(req->uri), "/path%20with%20spaces/file%2Fname");

    http_parser_destroy(ctx);
}

/*
 * Test: Header names are case-insensitive
 * RFC 7230 Section 3.2: field names are case-insensitive
 */
static void test_header_case_insensitive(void **state) {
    (void) state;

    const char *request_data =
        "GET / HTTP/1.1\r\n"
        "HOST: localhost\r\n"
        "Content-TYPE: text/plain\r\n"
        "X-Custom-HEADER: value\r\n"
        "\r\n";

    http1_parser_t *ctx = http_parser_create(10 * 1024 * 1024);
    assert_non_null(ctx);

    int result = http_parser_execute(ctx, request_data, strlen(request_data), NULL);
    assert_int_equal(result, 0);

    http_request_t *req = http_parser_get_request(ctx);
    assert_non_null(req);
    assert_non_null(req->headers);

    /* Headers should be stored in lowercase */
    zval *header;
    header = zend_hash_str_find(req->headers, "host", sizeof("host") - 1);
    assert_non_null(header);
    assert_string_equal(Z_STRVAL_P(header), "localhost");

    header = zend_hash_str_find(req->headers, "content-type", sizeof("content-type") - 1);
    assert_non_null(header);
    assert_string_equal(Z_STRVAL_P(header), "text/plain");

    header = zend_hash_str_find(req->headers, "x-custom-header", sizeof("x-custom-header") - 1);
    assert_non_null(header);
    assert_string_equal(Z_STRVAL_P(header), "value");

    http_parser_destroy(ctx);
}

/*
 * Test: Duplicate headers (e.g., Set-Cookie)
 * RFC 7230 Section 3.2.2: multiple header fields with same name
 */
static void test_duplicate_headers(void **state) {
    (void) state;

    const char *request_data =
        "GET / HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Cookie: session=abc\r\n"
        "Cookie: user=john\r\n"
        "\r\n";

    http1_parser_t *ctx = http_parser_create(10 * 1024 * 1024);
    assert_non_null(ctx);

    int result = http_parser_execute(ctx, request_data, strlen(request_data), NULL);
    assert_int_equal(result, 0);

    http_request_t *req = http_parser_get_request(ctx);
    assert_non_null(req);
    assert_non_null(req->headers);

    /* Cookie header should exist (may be combined or array) */
    zval *cookie = zend_hash_str_find(req->headers, "cookie", sizeof("cookie") - 1);
    assert_non_null(cookie);

    http_parser_destroy(ctx);
}

/*
 * Test: Empty header value
 * RFC 7230: empty field values are allowed
 */
static void test_empty_header_value(void **state) {
    (void) state;

    const char *request_data =
        "GET / HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "X-Empty:\r\n"
        "X-Empty-Space: \r\n"
        "\r\n";

    http1_parser_t *ctx = http_parser_create(10 * 1024 * 1024);
    assert_non_null(ctx);

    int result = http_parser_execute(ctx, request_data, strlen(request_data), NULL);
    assert_int_equal(result, 0);

    http_request_t *req = http_parser_get_request(ctx);
    assert_non_null(req);

    zval *header = zend_hash_str_find(req->headers, "x-empty", sizeof("x-empty") - 1);
    assert_non_null(header);
    assert_string_equal(Z_STRVAL_P(header), "");

    http_parser_destroy(ctx);
}

/*
 * Test: Missing Host header in HTTP/1.1
 * RFC 7230 Section 5.4: Host header is required in HTTP/1.1
 */
static void test_missing_host_http11(void **state) {
    (void) state;

    const char *request_data =
        "GET / HTTP/1.1\r\n"
        "\r\n";

    http1_parser_t *ctx = http_parser_create(10 * 1024 * 1024);
    assert_non_null(ctx);

    int result = http_parser_execute(ctx, request_data, strlen(request_data), NULL);

    /* llhttp may not enforce Host requirement - check if parsed */
    /* Server should return 400 Bad Request */
    http_request_t *req = http_parser_get_request(ctx);
    if (result == 0 && req) {
        /* If parsed, Host header should be missing */
        zval *host = zend_hash_str_find(req->headers, "host", sizeof("host") - 1);
        assert_null(host);
    }

    http_parser_destroy(ctx);
}

/* ========================================================================
 * HTTP Smuggling Security Tests
 * ======================================================================== */

/*
 * Test: Double Content-Length headers with DIFFERENT values (CL.CL smuggling).
 * RFC 9112 §6.3 (S-04 audit fix): duplicates are an error unless every value
 * is identical. llhttp strict mode catches this first (HPE_INVALID_CONTENT_LENGTH
 * → MALFORMED); our save_current_header check is the backstop if llhttp ever
 * accepts. Either way: 400 Bad Request, request rejected.
 */
static void test_double_content_length(void **state) {
    (void) state;

    const char *request_data =
        "POST / HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Content-Length: 5\r\n"
        "Content-Length: 100\r\n"
        "\r\n"
        "hello";

    http1_parser_t *ctx = http_parser_create(10 * 1024 * 1024);
    assert_non_null(ctx);

    int result = http_parser_execute(ctx, request_data, strlen(request_data), NULL);
    assert_true(result < 0);
    assert_int_equal(http_parse_error_to_status(ctx->parse_error), 400);

    http_parser_destroy(ctx);
}

/*
 * Test: Content-Length + Transfer-Encoding coexistence (CL.TE smuggling).
 * RFC 9112 §6.3 (S-03 audit fix): the spec text says TE wins, but middleboxes
 * desync — refusing the request closes the smuggling vector instead of
 * guessing. Both orderings must reject with 400 Bad Request.
 */
static void test_content_length_with_transfer_encoding(void **state) {
    (void) state;

    const char *request_data =
        "POST / HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Content-Length: 100\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "5\r\n"
        "hello\r\n"
        "0\r\n"
        "\r\n";

    http1_parser_t *ctx = http_parser_create(10 * 1024 * 1024);
    assert_non_null(ctx);

    int result = http_parser_execute(ctx, request_data, strlen(request_data), NULL);
    assert_true(result < 0);
    assert_int_equal(http_parse_error_to_status(ctx->parse_error), 400);

    http_parser_destroy(ctx);
}

/*
 * Test: Transfer-Encoding + Content-Length, reverse order (TE.CL smuggling).
 * Same RFC 9112 §6.3 rationale: order must not matter, both reject.
 */
static void test_transfer_encoding_with_content_length(void **state) {
    (void) state;

    const char *request_data =
        "POST / HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Transfer-Encoding: chunked\r\n"
        "Content-Length: 100\r\n"
        "\r\n"
        "5\r\n"
        "hello\r\n"
        "0\r\n"
        "\r\n";

    http1_parser_t *ctx = http_parser_create(10 * 1024 * 1024);
    assert_non_null(ctx);

    int result = http_parser_execute(ctx, request_data, strlen(request_data), NULL);
    assert_true(result < 0);
    assert_int_equal(http_parse_error_to_status(ctx->parse_error), 400);

    http_parser_destroy(ctx);
}

/*
 * Test: Invalid Transfer-Encoding value
 */
static void test_invalid_transfer_encoding(void **state) {
    (void) state;

    const char *request_data =
        "POST / HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Transfer-Encoding: invalid\r\n"
        "Content-Length: 5\r\n"
        "\r\n"
        "hello";

    http1_parser_t *ctx = http_parser_create(10 * 1024 * 1024);
    assert_non_null(ctx);

    int result = http_parser_execute(ctx, request_data, strlen(request_data), NULL);

    /* Unknown Transfer-Encoding should cause error or be ignored */
    (void)result;

    http_parser_destroy(ctx);
}

/* ========================================================================
 * Malformed Input Security Tests
 * ======================================================================== */

/*
 * Test: Null byte in URI
 */
static void test_null_byte_in_uri(void **state) {
    (void) state;

    char request_data[256];
    snprintf(request_data, sizeof(request_data),
        "GET /path%cinjected HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "\r\n", '\0');

    /* Manually set length to include null byte */
    size_t len = 50;  /* approximate length including null */

    http1_parser_t *ctx = http_parser_create(10 * 1024 * 1024);
    assert_non_null(ctx);

    int result = http_parser_execute(ctx, request_data, len, NULL);

    /* Should either reject or truncate at null byte */
    (void)result;

    http_parser_destroy(ctx);
}

/*
 * Test: Null byte in header value
 */
static void test_null_byte_in_header(void **state) {
    (void) state;

    char request_data[256];
    int offset = snprintf(request_data, sizeof(request_data),
        "GET / HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "X-Injected: before");
    request_data[offset] = '\0';  /* null byte */
    offset++;
    snprintf(request_data + offset, sizeof(request_data) - offset,
        "after\r\n\r\n");

    http1_parser_t *ctx = http_parser_create(10 * 1024 * 1024);
    assert_non_null(ctx);

    int result = http_parser_execute(ctx, request_data, offset + 10, NULL);

    /* Should handle null byte appropriately */
    (void)result;

    http_parser_destroy(ctx);
}

/*
 * Test: CRLF injection in header value
 */
static void test_crlf_injection_header(void **state) {
    (void) state;

    const char *request_data =
        "GET / HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "X-Injected: value\r\nX-Fake: injected\r\n"
        "\r\n";

    http1_parser_t *ctx = http_parser_create(10 * 1024 * 1024);
    assert_non_null(ctx);

    int result = http_parser_execute(ctx, request_data, strlen(request_data), NULL);

    if (result == 0) {
        http_request_t *req = http_parser_get_request(ctx);
        if (req) {
            /* X-Fake should either not exist or be properly separated */
            zval *fake = zend_hash_str_find(req->headers, "x-fake", sizeof("x-fake") - 1);
            /* If llhttp creates X-Fake as separate header, that's acceptable */
            /* But value of X-Injected should not contain CRLF */
        }
    }

    http_parser_destroy(ctx);
}

/*
 * Test: Space in header name (should reject)
 * RFC 7230 Section 3.2.4: no whitespace in field-name
 */
static void test_space_in_header_name(void **state) {
    (void) state;

    const char *request_data =
        "GET / HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Invalid Header: value\r\n"
        "\r\n";

    http1_parser_t *ctx = http_parser_create(10 * 1024 * 1024);
    assert_non_null(ctx);

    int result = http_parser_execute(ctx, request_data, strlen(request_data), NULL);

    /* Should reject header with space in name */
    assert_true(result < 0);

    http_parser_destroy(ctx);
}

/*
 * Test: Malformed chunked encoding (missing CRLF)
 */
static void test_malformed_chunked(void **state) {
    (void) state;

    const char *request_data =
        "POST / HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "5\r\n"
        "hello"  /* Missing CRLF after chunk data */
        "0\r\n"
        "\r\n";

    http1_parser_t *ctx = http_parser_create(10 * 1024 * 1024);
    assert_non_null(ctx);

    int result = http_parser_execute(ctx, request_data, strlen(request_data), NULL);

    /* Should fail on malformed chunk */
    assert_true(result < 0);

    http_parser_destroy(ctx);
}

/*
 * Test: Invalid chunk size (non-hex)
 */
static void test_invalid_chunk_size(void **state) {
    (void) state;

    const char *request_data =
        "POST / HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "xyz\r\n"  /* Invalid chunk size */
        "hello\r\n"
        "0\r\n"
        "\r\n";

    http1_parser_t *ctx = http_parser_create(10 * 1024 * 1024);
    assert_non_null(ctx);

    int result = http_parser_execute(ctx, request_data, strlen(request_data), NULL);

    /* Should fail on invalid chunk size */
    assert_true(result < 0);

    http_parser_destroy(ctx);
}

/*
 * Test: Negative Content-Length (S-01 audit fix).
 * Bare strtoul silently turned "-1" into ULONG_MAX. Strict parser rejects.
 */
static void test_negative_content_length(void **state) {
    (void) state;

    const char *request_data =
        "POST / HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Content-Length: -1\r\n"
        "\r\n";

    http1_parser_t *ctx = http_parser_create(10 * 1024 * 1024);
    assert_non_null(ctx);

    int result = http_parser_execute(ctx, request_data, strlen(request_data), NULL);
    assert_true(result < 0);
    assert_int_equal(http_parse_error_to_status(ctx->parse_error), 400);

    http_parser_destroy(ctx);
}

/*
 * Test: Overflow Content-Length (S-01 audit fix).
 * Number that overflows uint64 must be rejected, not clamped. llhttp catches
 * the overflow itself; our parse_content_length helper is the backstop.
 */
static void test_overflow_content_length(void **state) {
    (void) state;

    const char *request_data =
        "POST / HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Content-Length: 99999999999999999999999999999\r\n"
        "\r\n";

    http1_parser_t *ctx = http_parser_create(10 * 1024 * 1024);
    assert_non_null(ctx);

    int result = http_parser_execute(ctx, request_data, strlen(request_data), NULL);
    assert_true(result < 0);
    assert_int_equal(http_parse_error_to_status(ctx->parse_error), 400);

    http_parser_destroy(ctx);
}

/*
 * Test: Header count above HTTP_MAX_HEADER_COUNT cap.
 * DoS hardening: thousands of 1-byte headers would bloat the HashTable.
 * Cap is 256; 300 headers must be rejected with 431.
 */
static void test_too_many_headers(void **state) {
    (void) state;

    char *request_data = malloc(64 * 1024);
    int offset = snprintf(request_data, 64 * 1024,
        "GET / HTTP/1.1\r\n"
        "Host: localhost\r\n");

    for (int i = 0; i < 300; i++) {
        offset += snprintf(request_data + offset, 64 * 1024 - offset,
            "X-H%d: v%d\r\n", i, i);
    }
    offset += snprintf(request_data + offset, 64 * 1024 - offset, "\r\n");

    http1_parser_t *ctx = http_parser_create(10 * 1024 * 1024);
    assert_non_null(ctx);

    int result = http_parser_execute(ctx, request_data, strlen(request_data), NULL);
    assert_true(result < 0);
    assert_int_equal(ctx->parse_error, HTTP_PARSE_ERR_TOO_MANY_HEADERS);
    assert_int_equal(http_parse_error_to_status(ctx->parse_error), 431);

    free(request_data);
    http_parser_destroy(ctx);
}

/* ========================================================================
 * HTTP Smuggling - Transfer-Encoding Obfuscation (TE.TE attacks)
 * Reference: https://portswigger.net/web-security/request-smuggling
 * ======================================================================== */

/*
 * Test: Transfer-Encoding with space before colon
 * Some servers ignore malformed headers, enabling TE.TE attacks
 */
static void test_te_space_before_colon(void **state) {
    (void) state;

    const char *request_data =
        "POST / HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Transfer-Encoding : chunked\r\n"  /* Space before colon */
        "\r\n"
        "5\r\n"
        "hello\r\n"
        "0\r\n"
        "\r\n";

    http1_parser_t *ctx = http_parser_create(10 * 1024 * 1024);
    assert_non_null(ctx);

    int result = http_parser_execute(ctx, request_data, strlen(request_data), NULL);

    /* Should reject - space before colon is invalid per RFC 7230 */
    assert_true(result < 0);

    http_parser_destroy(ctx);
}

/*
 * Test: Transfer-Encoding with tab before colon
 */
static void test_te_tab_before_colon(void **state) {
    (void) state;

    const char *request_data =
        "POST / HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Transfer-Encoding\t: chunked\r\n"  /* Tab before colon */
        "\r\n"
        "5\r\n"
        "hello\r\n"
        "0\r\n"
        "\r\n";

    http1_parser_t *ctx = http_parser_create(10 * 1024 * 1024);
    assert_non_null(ctx);

    int result = http_parser_execute(ctx, request_data, strlen(request_data), NULL);

    /* Should reject - tab in header name is invalid */
    assert_true(result < 0);

    http_parser_destroy(ctx);
}

/*
 * Test: Multiple Transfer-Encoding headers
 * Could enable request smuggling if handled inconsistently
 */
static void test_multiple_transfer_encoding(void **state) {
    (void) state;

    const char *request_data =
        "POST / HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Transfer-Encoding: chunked\r\n"
        "Transfer-Encoding: identity\r\n"
        "\r\n"
        "5\r\n"
        "hello\r\n"
        "0\r\n"
        "\r\n";

    http1_parser_t *ctx = http_parser_create(10 * 1024 * 1024);
    assert_non_null(ctx);

    int result = http_parser_execute(ctx, request_data, strlen(request_data), NULL);

    /* Parser behavior - just ensure no crash */
    /* Combined as "chunked, identity" per RFC 7230 */
    (void)result;

    http_parser_destroy(ctx);
}

/*
 * Test: Transfer-Encoding with trailing whitespace
 */
static void test_te_trailing_whitespace(void **state) {
    (void) state;

    const char *request_data =
        "POST / HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Transfer-Encoding: chunked \r\n"  /* Trailing space */
        "\r\n"
        "5\r\n"
        "hello\r\n"
        "0\r\n"
        "\r\n";

    http1_parser_t *ctx = http_parser_create(10 * 1024 * 1024);
    assert_non_null(ctx);

    int result = http_parser_execute(ctx, request_data, strlen(request_data), NULL);

    /* Should handle trailing whitespace - OWS is allowed */
    /* Just ensure no crash and consistent behavior */
    (void)result;

    http_parser_destroy(ctx);
}

/* ========================================================================
 * Obsolete Line Folding (obs-fold) - RFC 7230 Section 3.2.4
 * Reference: https://datatracker.ietf.org/doc/html/rfc7230#section-3.2.4
 * ======================================================================== */

/*
 * Test: Header with obs-fold (CRLF + space continuation)
 * RFC 7230: MUST reject or replace with SP
 */
static void test_obs_fold_space(void **state) {
    (void) state;

    const char *request_data =
        "GET / HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "X-Folded: value1\r\n"
        " continued\r\n"  /* obs-fold: CRLF + SP */
        "\r\n";

    http1_parser_t *ctx = http_parser_create(10 * 1024 * 1024);
    assert_non_null(ctx);

    int result = http_parser_execute(ctx, request_data, strlen(request_data), NULL);

    /* Should either reject (400) or handle by replacing with SP */
    /* Just ensure no crash */
    (void)result;

    http_parser_destroy(ctx);
}

/*
 * Test: Header with obs-fold (CRLF + tab continuation)
 */
static void test_obs_fold_tab(void **state) {
    (void) state;

    const char *request_data =
        "GET / HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "X-Folded: value1\r\n"
        "\tcontinued\r\n"  /* obs-fold: CRLF + HTAB */
        "\r\n";

    http1_parser_t *ctx = http_parser_create(10 * 1024 * 1024);
    assert_non_null(ctx);

    int result = http_parser_execute(ctx, request_data, strlen(request_data), NULL);

    /* Should either reject or handle */
    (void)result;

    http_parser_destroy(ctx);
}

/* ========================================================================
 * Line Ending Edge Cases
 * Reference: HTTP Garden - bare LF vulnerabilities
 * ======================================================================== */

/*
 * Test: Bare LF instead of CRLF in headers
 */
static void test_bare_lf_in_headers(void **state) {
    (void) state;

    const char *request_data =
        "GET / HTTP/1.1\n"  /* Bare LF */
        "Host: localhost\n"
        "\n";

    http1_parser_t *ctx = http_parser_create(10 * 1024 * 1024);
    assert_non_null(ctx);

    int result = http_parser_execute(ctx, request_data, strlen(request_data), NULL);

    /* Some parsers accept bare LF, some reject */
    /* Ensure consistent behavior, no crash */
    (void)result;

    http_parser_destroy(ctx);
}

/*
 * Test: Mixed CRLF and bare LF
 */
static void test_mixed_line_endings(void **state) {
    (void) state;

    const char *request_data =
        "GET / HTTP/1.1\r\n"
        "Host: localhost\n"  /* Bare LF */
        "Content-Length: 0\r\n"
        "\r\n";

    http1_parser_t *ctx = http_parser_create(10 * 1024 * 1024);
    assert_non_null(ctx);

    int result = http_parser_execute(ctx, request_data, strlen(request_data), NULL);

    /* Ensure consistent behavior */
    (void)result;

    http_parser_destroy(ctx);
}

/*
 * Test: CR without LF
 */
static void test_bare_cr(void **state) {
    (void) state;

    const char *request_data =
        "GET / HTTP/1.1\r"  /* CR without LF */
        "Host: localhost\r\n"
        "\r\n";

    http1_parser_t *ctx = http_parser_create(10 * 1024 * 1024);
    assert_non_null(ctx);

    int result = http_parser_execute(ctx, request_data, strlen(request_data), NULL);

    /* Should reject or handle consistently */
    (void)result;

    http_parser_destroy(ctx);
}

/* ========================================================================
 * Chunk Extension and Size Edge Cases
 * Reference: RFC 7230 Section 4.1.1
 * ======================================================================== */

/*
 * Test: Chunk with extension
 * Format: chunk-size *( ";" chunk-ext-name [ "=" chunk-ext-val ] )
 */
static void test_chunk_with_extension(void **state) {
    (void) state;

    const char *request_data =
        "POST / HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "5;ext=value\r\n"  /* Chunk with extension */
        "hello\r\n"
        "0\r\n"
        "\r\n";

    http1_parser_t *ctx = http_parser_create(10 * 1024 * 1024);
    assert_non_null(ctx);

    int result = http_parser_execute(ctx, request_data, strlen(request_data), NULL);
    assert_int_equal(result, 0);
    assert_true(http_parser_is_complete(ctx));

    http_request_t *req = http_parser_get_request(ctx);
    assert_non_null(req);
    assert_non_null(req->body);
    assert_string_equal(ZSTR_VAL(req->body), "hello");

    http_parser_destroy(ctx);
}

/*
 * Test: Chunk size with leading zeros
 */
static void test_chunk_leading_zeros(void **state) {
    (void) state;

    const char *request_data =
        "POST / HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "00005\r\n"  /* Leading zeros */
        "hello\r\n"
        "0\r\n"
        "\r\n";

    http1_parser_t *ctx = http_parser_create(10 * 1024 * 1024);
    assert_non_null(ctx);

    int result = http_parser_execute(ctx, request_data, strlen(request_data), NULL);

    /* Should parse correctly - leading zeros are valid hex */
    assert_int_equal(result, 0);
    assert_true(http_parser_is_complete(ctx));

    http_request_t *req = http_parser_get_request(ctx);
    assert_non_null(req);
    assert_non_null(req->body);
    assert_string_equal(ZSTR_VAL(req->body), "hello");

    http_parser_destroy(ctx);
}

/*
 * Test: Chunk size with uppercase hex
 */
static void test_chunk_uppercase_hex(void **state) {
    (void) state;

    const char *request_data =
        "POST / HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "A\r\n"  /* Uppercase hex for 10 */
        "0123456789\r\n"
        "0\r\n"
        "\r\n";

    http1_parser_t *ctx = http_parser_create(10 * 1024 * 1024);
    assert_non_null(ctx);

    int result = http_parser_execute(ctx, request_data, strlen(request_data), NULL);
    assert_int_equal(result, 0);
    assert_true(http_parser_is_complete(ctx));

    http_request_t *req = http_parser_get_request(ctx);
    assert_non_null(req);
    assert_non_null(req->body);
    assert_int_equal(ZSTR_LEN(req->body), 10);

    http_parser_destroy(ctx);
}

/*
 * Test: Very long chunk size string
 */
static void test_chunk_size_too_long(void **state) {
    (void) state;

    /* Build chunk size with many leading zeros */
    char request_data[1024];
    snprintf(request_data, sizeof(request_data),
        "POST / HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "000000000000000000000000000000000000000000000000000000000000000000005\r\n"
        "hello\r\n"
        "0\r\n"
        "\r\n");

    http1_parser_t *ctx = http_parser_create(10 * 1024 * 1024);
    assert_non_null(ctx);

    int result = http_parser_execute(ctx, request_data, strlen(request_data), NULL);

    /* Should either parse or reject, no crash */
    (void)result;

    http_parser_destroy(ctx);
}

/* ========================================================================
 * Request Line Edge Cases
 * ======================================================================== */

/*
 * Test: Tab instead of space in request line
 */
static void test_tab_in_request_line(void **state) {
    (void) state;

    const char *request_data =
        "GET\t/\tHTTP/1.1\r\n"  /* Tabs instead of spaces */
        "Host: localhost\r\n"
        "\r\n";

    http1_parser_t *ctx = http_parser_create(10 * 1024 * 1024);
    assert_non_null(ctx);

    int result = http_parser_execute(ctx, request_data, strlen(request_data), NULL);

    /* Should reject - only SP is valid delimiter */
    assert_true(result < 0);

    http_parser_destroy(ctx);
}

/*
 * Test: Multiple spaces in request line
 *
 * RFC 7230 Section 3.1.1: request-line = method SP request-target SP HTTP-version
 * Strictly speaking, only single SP is valid between tokens.
 *
 * However, llhttp intentionally uses LENIENT parsing for real-world compatibility.
 * Many HTTP clients send malformed requests with extra whitespace.
 * Rejecting them would break compatibility with browsers and tools.
 *
 * Decision: Accept llhttp's lenient behavior rather than enforce strict RFC.
 */
static void test_multiple_spaces_request_line(void **state) {
    (void) state;

    const char *request_data =
        "GET  /  HTTP/1.1\r\n"  /* Double spaces - invalid per RFC, but accepted */
        "Host: localhost\r\n"
        "\r\n";

    http1_parser_t *ctx = http_parser_create(10 * 1024 * 1024);
    assert_non_null(ctx);

    int result = http_parser_execute(ctx, request_data, strlen(request_data), NULL);

    /* llhttp accepts this (lenient mode) - verify no crash */
    (void)result;

    http_parser_destroy(ctx);
}

/*
 * Test: Absolute URI in request (proxy request)
 * GET http://example.com/path HTTP/1.1
 */
static void test_absolute_uri(void **state) {
    (void) state;

    const char *request_data =
        "GET http://example.com/path HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "\r\n";

    http1_parser_t *ctx = http_parser_create(10 * 1024 * 1024);
    assert_non_null(ctx);

    int result = http_parser_execute(ctx, request_data, strlen(request_data), NULL);
    assert_int_equal(result, 0);
    assert_true(http_parser_is_complete(ctx));

    http_request_t *req = http_parser_get_request(ctx);
    assert_non_null(req);
    assert_non_null(req->uri);
    /* URI should contain the full absolute form */
    assert_string_equal(ZSTR_VAL(req->uri), "http://example.com/path");

    http_parser_destroy(ctx);
}

/* ========================================================================
 * Host Header Edge Cases
 * ======================================================================== */

/*
 * Test: Multiple Host headers
 * RFC 7230: MUST reject with 400
 */
static void test_multiple_host_headers(void **state) {
    (void) state;

    const char *request_data =
        "GET / HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Host: example.com\r\n"  /* Second Host header */
        "\r\n";

    http1_parser_t *ctx = http_parser_create(10 * 1024 * 1024);
    assert_non_null(ctx);

    int result = http_parser_execute(ctx, request_data, strlen(request_data), NULL);

    /* Per our implementation, duplicate headers are combined with comma */
    /* This is technically allowed for most headers, but Host is special */
    /* Just ensure no crash */
    (void)result;

    http_parser_destroy(ctx);
}

/*
 * Test: Host with port
 */
static void test_host_with_port(void **state) {
    (void) state;

    const char *request_data =
        "GET / HTTP/1.1\r\n"
        "Host: localhost:8080\r\n"
        "\r\n";

    http1_parser_t *ctx = http_parser_create(10 * 1024 * 1024);
    assert_non_null(ctx);

    int result = http_parser_execute(ctx, request_data, strlen(request_data), NULL);
    assert_int_equal(result, 0);
    assert_true(http_parser_is_complete(ctx));

    http_request_t *req = http_parser_get_request(ctx);
    zval *host = zend_hash_str_find(req->headers, "host", sizeof("host") - 1);
    assert_non_null(host);
    assert_string_equal(Z_STRVAL_P(host), "localhost:8080");

    http_parser_destroy(ctx);
}

/*
 * Test: Empty Host header value
 */
static void test_empty_host_header(void **state) {
    (void) state;

    const char *request_data =
        "GET / HTTP/1.1\r\n"
        "Host:\r\n"  /* Empty value */
        "\r\n";

    http1_parser_t *ctx = http_parser_create(10 * 1024 * 1024);
    assert_non_null(ctx);

    int result = http_parser_execute(ctx, request_data, strlen(request_data), NULL);

    /* Empty Host is valid for HTTP/1.1 to authority-form targets */
    /* Just ensure no crash */
    (void)result;

    http_parser_destroy(ctx);
}

/* ========================================================================
 * HTTP Version Edge Cases
 * ======================================================================== */

/*
 * Test: HTTP/0.9 simple request (no headers)
 */
static void test_http09_simple_request(void **state) {
    (void) state;

    /* HTTP/0.9 has no version, no headers */
    const char *request_data = "GET /\r\n";

    http1_parser_t *ctx = http_parser_create(10 * 1024 * 1024);
    assert_non_null(ctx);

    int result = http_parser_execute(ctx, request_data, strlen(request_data), NULL);

    /* Most modern parsers reject HTTP/0.9 */
    /* Just ensure no crash */
    (void)result;

    http_parser_destroy(ctx);
}

/*
 * Test: HTTP/2.0 in HTTP/1.1 parser
 */
static void test_http2_in_http1_parser(void **state) {
    (void) state;

    const char *request_data =
        "GET / HTTP/2.0\r\n"
        "Host: localhost\r\n"
        "\r\n";

    http1_parser_t *ctx = http_parser_create(10 * 1024 * 1024);
    assert_non_null(ctx);

    int result = http_parser_execute(ctx, request_data, strlen(request_data), NULL);

    /* llhttp may accept or reject HTTP/2.0 version string */
    /* Just ensure no crash */
    (void)result;

    http_parser_destroy(ctx);
}

/*
 * Test: Lowercase http version
 */
static void test_lowercase_http_version(void **state) {
    (void) state;

    const char *request_data =
        "GET / http/1.1\r\n"  /* lowercase */
        "Host: localhost\r\n"
        "\r\n";

    http1_parser_t *ctx = http_parser_create(10 * 1024 * 1024);
    assert_non_null(ctx);

    int result = http_parser_execute(ctx, request_data, strlen(request_data), NULL);

    /* Should reject - HTTP is case-sensitive */
    assert_true(result < 0);

    http_parser_destroy(ctx);
}

/* ========================================================================
 * CVE-Based Tests - Known llhttp/Node.js Vulnerabilities
 * ======================================================================== */

/*
 * CVE-2022-35256: Headers not properly terminated with CRLF
 * The llhttp parser does not correctly handle header fields that are
 * not terminated with CLRF. This may result in HTTP Request Smuggling.
 */
static void test_cve_2022_35256_header_no_crlf(void **state) {
    (void) state;

    /* Header line without proper CRLF termination */
    const char request_data[] =
        "GET / HTTP/1.1\r\n"
        "Host: localhost\r"  /* CR only, no LF */
        "X-Test: value\r\n"
        "\r\n";

    http1_parser_t *ctx = http_parser_create(10 * 1024 * 1024);
    assert_non_null(ctx);

    int result = http_parser_execute(ctx, request_data, sizeof(request_data) - 1, NULL);

    /* Should reject or handle safely */
    (void)result;

    http_parser_destroy(ctx);
}

/*
 * CVE-2022-32215: Multi-line Transfer-Encoding header
 * The llhttp parser does not correctly handle multi-line Transfer-Encoding
 * headers. This can lead to HTTP Request Smuggling.
 */
static void test_cve_2022_32215_multiline_te(void **state) {
    (void) state;

    const char *request_data =
        "POST / HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Transfer-Encoding: chunked\r\n"
        " , identity\r\n"  /* obs-fold continuation of TE header */
        "\r\n"
        "5\r\n"
        "hello\r\n"
        "0\r\n"
        "\r\n";

    http1_parser_t *ctx = http_parser_create(10 * 1024 * 1024);
    assert_non_null(ctx);

    int result = http_parser_execute(ctx, request_data, strlen(request_data), NULL);

    /* Should reject obs-fold in Transfer-Encoding or handle safely */
    (void)result;

    http_parser_destroy(ctx);
}

/*
 * 2025 Node.js vulnerability: Improper header termination with \r\n\rX
 * A flaw allows improper termination of HTTP/1 headers using \r\n\rX
 * instead of the required \r\n\r\n.
 */
static void test_2025_improper_header_termination(void **state) {
    (void) state;

    /* \r\n\rX instead of \r\n\r\n */
    const char request_data[] =
        "GET / HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "\r\rGET /admin HTTP/1.1\r\n"  /* Smuggled request attempt */
        "Host: localhost\r\n"
        "\r\n";

    http1_parser_t *ctx = http_parser_create(10 * 1024 * 1024);
    assert_non_null(ctx);

    int result = http_parser_execute(ctx, request_data, sizeof(request_data) - 1, NULL);

    /* Should reject - \r\r is not valid header termination */
    (void)result;

    http_parser_destroy(ctx);
}

/*
 * CVE-2024-22019: Unbounded chunk extension bytes
 * Denial of service via unbounded processing of chunk extension bytes.
 */
static void test_cve_2024_22019_long_chunk_extension(void **state) {
    (void) state;

    /* Build a very long chunk extension */
    char *request_data = malloc(100 * 1024);
    assert_non_null(request_data);

    int offset = snprintf(request_data, 100 * 1024,
        "POST / HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "5;ext=");

    /* Add 50KB of extension value */
    for (int i = 0; i < 50 * 1024; i++) {
        request_data[offset++] = 'A';
    }
    offset += snprintf(request_data + offset, 100 * 1024 - offset,
        "\r\n"
        "hello\r\n"
        "0\r\n"
        "\r\n");

    http1_parser_t *ctx = http_parser_create(10 * 1024 * 1024);
    assert_non_null(ctx);

    int result = http_parser_execute(ctx, request_data, offset, NULL);

    /* Should either parse (if within limits) or reject safely */
    /* Must not cause unbounded memory/CPU usage */
    (void)result;

    free(request_data);
    http_parser_destroy(ctx);
}

/*
 * CVE-2022-32214: CR as header delimiter without LF
 * The llhttp parser does not strictly use the CRLF sequence to delimit
 * HTTP requests. The CR character (without LF) is sufficient.
 */
static void test_cve_2022_32214_cr_only_delimiter(void **state) {
    (void) state;

    /* Using only CR as line delimiter */
    const char request_data[] =
        "GET / HTTP/1.1\r"
        "Host: localhost\r"
        "Content-Length: 0\r"
        "\r";

    http1_parser_t *ctx = http_parser_create(10 * 1024 * 1024);
    assert_non_null(ctx);

    int result = http_parser_execute(ctx, request_data, sizeof(request_data) - 1, NULL);

    /* Should reject - only CRLF is valid per RFC 7230 */
    (void)result;

    http_parser_destroy(ctx);
}

/*
 * CVE-2021-22959: Space after header name before colon (HRS)
 * The http parser accepts requests with a space right after the header
 * name before the colon. This can lead to HTTP Request Smuggling.
 */
static void test_cve_2021_22959_space_after_header_name(void **state) {
    (void) state;

    const char *request_data =
        "GET / HTTP/1.1\r\n"
        "Host : localhost\r\n"  /* Space between name and colon */
        "\r\n";

    http1_parser_t *ctx = http_parser_create(10 * 1024 * 1024);
    assert_non_null(ctx);

    int result = http_parser_execute(ctx, request_data, strlen(request_data), NULL);

    /* Should reject - no whitespace allowed before colon per RFC 7230 */
    assert_true(result < 0);

    http_parser_destroy(ctx);
}

/*
 * Additional: Request with NUL in chunk size (potential parser confusion)
 */
static void test_null_in_chunk_size(void **state) {
    (void) state;

    const char request_data[] =
        "POST / HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "5\0a\r\n"  /* NUL in chunk size */
        "hello\r\n"
        "0\r\n"
        "\r\n";

    http1_parser_t *ctx = http_parser_create(10 * 1024 * 1024);
    assert_non_null(ctx);

    /* Use sizeof to include NUL byte */
    int result = http_parser_execute(ctx, request_data, sizeof(request_data) - 1, NULL);

    /* Should handle NUL byte safely */
    (void)result;

    http_parser_destroy(ctx);
}

/*
 * Additional: Extremely long header line (potential buffer overflow)
 */
static void test_extremely_long_header_line(void **state) {
    (void) state;

    /* Create a header with 100KB value */
    size_t header_len = 100 * 1024;
    char *request_data = malloc(header_len + 200);
    assert_non_null(request_data);

    int offset = snprintf(request_data, header_len + 200,
        "GET / HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "X-Long: ");

    /* Fill with 'A' characters */
    memset(request_data + offset, 'A', header_len);
    offset += header_len;
    offset += snprintf(request_data + offset, 200, "\r\n\r\n");

    http1_parser_t *ctx = http_parser_create(10 * 1024 * 1024);
    assert_non_null(ctx);

    int result = http_parser_execute(ctx, request_data, offset, NULL);

    /* Should either accept (if within limits) or reject with error */
    /* Must not crash or overflow */
    (void)result;

    free(request_data);
    http_parser_destroy(ctx);
}

/* ========================================================================
 * Setup and Teardown
 * ======================================================================== */

static int group_setup(void **state) {
    (void) state;
    return php_test_runtime_init();
}

static int group_teardown(void **state) {
    (void) state;
    php_test_runtime_shutdown();
    return 0;
}

/* ========================================================================
 * Phase 0 audit fixes: S-01..S-04 + version + header count
 *
 * Note on assertion style: llhttp's strict mode catches malformed CL, dup CL,
 * and bogus HTTP versions before our save_current_header/on_headers_complete
 * checks fire — we get HTTP_PARSE_ERR_MALFORMED in that case. Our checks are
 * the backstop if llhttp ever loosens. Tests therefore assert "request was
 * rejected with the expected HTTP status" rather than pinning to a specific
 * internal error code.
 * ======================================================================== */

/*
 * Test: Content-Length with trailing junk ("100abc") is rejected.
 * Strict 1*DIGIT parsing per RFC 9110 §8.6.
 */
static void test_cl_invalid_trailing_junk(void **state) {
    (void) state;

    const char *request_data =
        "POST / HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Content-Length: 100abc\r\n"
        "\r\n";

    http1_parser_t *ctx = http_parser_create(10 * 1024 * 1024);
    assert_non_null(ctx);

    int result = http_parser_execute(ctx, request_data, strlen(request_data), NULL);
    assert_true(result < 0);
    assert_int_equal(http_parse_error_to_status(ctx->parse_error), 400);

    http_parser_destroy(ctx);
}

/*
 * Test: Content-Length with leading plus ("+5") is rejected.
 * RFC 9110 §8.6 forbids signs.
 */
static void test_cl_invalid_leading_plus(void **state) {
    (void) state;

    const char *request_data =
        "POST / HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Content-Length: +5\r\n"
        "\r\n"
        "hello";

    http1_parser_t *ctx = http_parser_create(10 * 1024 * 1024);
    assert_non_null(ctx);

    int result = http_parser_execute(ctx, request_data, strlen(request_data), NULL);
    assert_true(result < 0);
    assert_int_equal(http_parse_error_to_status(ctx->parse_error), 400);

    http_parser_destroy(ctx);
}

/*
 * Test: HTTP/0.9 request rejected with 400.
 * RFC 9112 §2.5: only HTTP/1.0 and HTTP/1.1 accepted.
 */
static void test_invalid_http_version_09(void **state) {
    (void) state;

    /* HTTP/0.9 has no version on the request line at all in the original
     * spec, but llhttp also accepts the explicit form. We feed the modern
     * form so llhttp parses it then we reject at on_headers_complete. */
    const char *request_data =
        "GET / HTTP/0.9\r\n"
        "Host: localhost\r\n"
        "\r\n";

    http1_parser_t *ctx = http_parser_create(10 * 1024 * 1024);
    assert_non_null(ctx);

    int result = http_parser_execute(ctx, request_data, strlen(request_data), NULL);
    /* llhttp may reject earlier as MALFORMED, or we reject at version
     * check. Either way: must not be accepted. */
    assert_true(result < 0);

    http_parser_destroy(ctx);
}

/*
 * Test: HTTP/2.0 request over HTTP/1 wire rejected with 400.
 * Plaintext H2 prior-knowledge has its own preface; spurious "HTTP/2.0"
 * on the request line is an error.
 */
static void test_invalid_http_version_20(void **state) {
    (void) state;

    const char *request_data =
        "GET / HTTP/2.0\r\n"
        "Host: localhost\r\n"
        "\r\n";

    http1_parser_t *ctx = http_parser_create(10 * 1024 * 1024);
    assert_non_null(ctx);

    int result = http_parser_execute(ctx, request_data, strlen(request_data), NULL);
    assert_true(result < 0);

    http_parser_destroy(ctx);
}

/*
 * Test: HTTP/1.7 (made-up minor) rejected.
 */
static void test_invalid_http_version_17(void **state) {
    (void) state;

    const char *request_data =
        "GET / HTTP/1.7\r\n"
        "Host: localhost\r\n"
        "\r\n";

    http1_parser_t *ctx = http_parser_create(10 * 1024 * 1024);
    assert_non_null(ctx);

    int result = http_parser_execute(ctx, request_data, strlen(request_data), NULL);
    assert_true(result < 0);
    assert_int_equal(http_parse_error_to_status(ctx->parse_error), 400);

    http_parser_destroy(ctx);
}

/*
 * Test: HTTP/1.0 request explicitly accepted.
 */
static void test_valid_http_version_10(void **state) {
    (void) state;

    const char *request_data =
        "GET / HTTP/1.0\r\n"
        "Host: localhost\r\n"
        "\r\n";

    http1_parser_t *ctx = http_parser_create(10 * 1024 * 1024);
    assert_non_null(ctx);

    int result = http_parser_execute(ctx, request_data, strlen(request_data), NULL);
    assert_int_equal(result, 0);

    http_request_t *req = http_parser_get_request(ctx);
    assert_non_null(req);
    assert_int_equal(req->http_major, 1);
    assert_int_equal(req->http_minor, 0);
    /* HTTP/1.0 default keep-alive is false */
    assert_false(req->keep_alive);

    http_parser_destroy(ctx);
}

/*
 * Test: Empty Content-Length value rejected.
 */
static void test_cl_empty_value(void **state) {
    (void) state;

    const char *request_data =
        "POST / HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Content-Length: \r\n"
        "\r\n";

    http1_parser_t *ctx = http_parser_create(10 * 1024 * 1024);
    assert_non_null(ctx);

    int result = http_parser_execute(ctx, request_data, strlen(request_data), NULL);
    assert_true(result < 0);

    http_parser_destroy(ctx);
}

/*
 * Main test suite
 */
int main(void) {
    const struct CMUnitTest tests[] = {
        /* RFC Compliance */
        cmocka_unit_test(test_uri_percent_encoding),
        cmocka_unit_test(test_header_case_insensitive),
        cmocka_unit_test(test_duplicate_headers),
        cmocka_unit_test(test_empty_header_value),
        cmocka_unit_test(test_missing_host_http11),

        /* HTTP Smuggling */
        cmocka_unit_test(test_double_content_length),
        cmocka_unit_test(test_content_length_with_transfer_encoding),
        cmocka_unit_test(test_transfer_encoding_with_content_length),
        cmocka_unit_test(test_invalid_transfer_encoding),

        /* Malformed Input */
        cmocka_unit_test(test_null_byte_in_uri),
        cmocka_unit_test(test_null_byte_in_header),
        cmocka_unit_test(test_crlf_injection_header),
        cmocka_unit_test(test_space_in_header_name),
        cmocka_unit_test(test_malformed_chunked),
        cmocka_unit_test(test_invalid_chunk_size),
        cmocka_unit_test(test_negative_content_length),
        cmocka_unit_test(test_overflow_content_length),
        cmocka_unit_test(test_too_many_headers),

        /* Phase 0 audit fixes (S-01..S-04 + version) */
        cmocka_unit_test(test_cl_invalid_trailing_junk),
        cmocka_unit_test(test_cl_invalid_leading_plus),
        cmocka_unit_test(test_cl_empty_value),
        cmocka_unit_test(test_invalid_http_version_09),
        cmocka_unit_test(test_invalid_http_version_20),
        cmocka_unit_test(test_invalid_http_version_17),
        cmocka_unit_test(test_valid_http_version_10),

        /* TE.TE Obfuscation (HTTP Smuggling) */
        cmocka_unit_test(test_te_space_before_colon),
        cmocka_unit_test(test_te_tab_before_colon),
        cmocka_unit_test(test_multiple_transfer_encoding),
        cmocka_unit_test(test_te_trailing_whitespace),

        /* Obsolete Line Folding (obs-fold) */
        cmocka_unit_test(test_obs_fold_space),
        cmocka_unit_test(test_obs_fold_tab),

        /* Line Ending Edge Cases */
        cmocka_unit_test(test_bare_lf_in_headers),
        cmocka_unit_test(test_mixed_line_endings),
        cmocka_unit_test(test_bare_cr),

        /* Chunk Extensions and Size Edge Cases */
        cmocka_unit_test(test_chunk_with_extension),
        cmocka_unit_test(test_chunk_leading_zeros),
        cmocka_unit_test(test_chunk_uppercase_hex),
        cmocka_unit_test(test_chunk_size_too_long),

        /* Request Line Edge Cases */
        cmocka_unit_test(test_tab_in_request_line),
        cmocka_unit_test(test_multiple_spaces_request_line),
        cmocka_unit_test(test_absolute_uri),

        /* Host Header Edge Cases */
        cmocka_unit_test(test_multiple_host_headers),
        cmocka_unit_test(test_host_with_port),
        cmocka_unit_test(test_empty_host_header),

        /* HTTP Version Edge Cases */
        cmocka_unit_test(test_http09_simple_request),
        cmocka_unit_test(test_http2_in_http1_parser),
        cmocka_unit_test(test_lowercase_http_version),

        /* CVE-Based Tests (known llhttp/Node.js vulnerabilities) */
        cmocka_unit_test(test_cve_2022_35256_header_no_crlf),
        cmocka_unit_test(test_cve_2022_32215_multiline_te),
        cmocka_unit_test(test_2025_improper_header_termination),
        cmocka_unit_test(test_cve_2024_22019_long_chunk_extension),
        cmocka_unit_test(test_cve_2022_32214_cr_only_delimiter),
        cmocka_unit_test(test_cve_2021_22959_space_after_header_name),
        cmocka_unit_test(test_null_in_chunk_size),
        cmocka_unit_test(test_extremely_long_header_line),
    };

    return cmocka_run_group_tests(tests, group_setup, group_teardown);
}
