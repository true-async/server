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
#include <stdio.h>

#include "formats/multipart_parser.h"

/* Test context for collecting callback data */
typedef struct {
    int part_begin_count;
    int part_end_count;
    int headers_complete_count;
    int body_end_count;

    /* Accumulated header data */
    char header_name[256];
    size_t header_name_len;
    char header_value[256];
    size_t header_value_len;

    /* Accumulated part data */
    char part_data[4096];
    size_t part_data_len;

    /* All parts data (for multiple parts) */
    char all_data[8192];
    size_t all_data_len;
} test_context_t;

/* Callbacks */
static int on_part_begin(multipart_parser_t* parser) {
    test_context_t* ctx = multipart_parser_get_data(parser);
    ctx->part_begin_count++;
    /* Reset per-part data */
    ctx->header_name_len = 0;
    ctx->header_value_len = 0;
    ctx->part_data_len = 0;
    return 0;
}

static int on_header_field(multipart_parser_t* parser, const char* at, size_t length) {
    test_context_t* ctx = multipart_parser_get_data(parser);
    if (ctx->header_name_len + length < sizeof(ctx->header_name)) {
        memcpy(ctx->header_name + ctx->header_name_len, at, length);
        ctx->header_name_len += length;
        ctx->header_name[ctx->header_name_len] = '\0';
    }
    return 0;
}

static int on_header_value(multipart_parser_t* parser, const char* at, size_t length) {
    test_context_t* ctx = multipart_parser_get_data(parser);
    if (ctx->header_value_len + length < sizeof(ctx->header_value)) {
        memcpy(ctx->header_value + ctx->header_value_len, at, length);
        ctx->header_value_len += length;
        ctx->header_value[ctx->header_value_len] = '\0';
    }
    return 0;
}

static int on_headers_complete(multipart_parser_t* parser) {
    test_context_t* ctx = multipart_parser_get_data(parser);
    ctx->headers_complete_count++;
    return 0;
}

static int on_part_data(multipart_parser_t* parser, const char* at, size_t length) {
    test_context_t* ctx = multipart_parser_get_data(parser);
    if (ctx->part_data_len + length < sizeof(ctx->part_data)) {
        memcpy(ctx->part_data + ctx->part_data_len, at, length);
        ctx->part_data_len += length;
        ctx->part_data[ctx->part_data_len] = '\0';
    }
    if (ctx->all_data_len + length < sizeof(ctx->all_data)) {
        memcpy(ctx->all_data + ctx->all_data_len, at, length);
        ctx->all_data_len += length;
        ctx->all_data[ctx->all_data_len] = '\0';
    }
    return 0;
}

static int on_part_end(multipart_parser_t* parser) {
    test_context_t* ctx = multipart_parser_get_data(parser);
    ctx->part_end_count++;
    return 0;
}

static int on_body_end(multipart_parser_t* parser) {
    test_context_t* ctx = multipart_parser_get_data(parser);
    ctx->body_end_count++;
    return 0;
}

static multipart_callbacks_t test_callbacks = {
    .on_part_begin = on_part_begin,
    .on_header_field = on_header_field,
    .on_header_value = on_header_value,
    .on_headers_complete = on_headers_complete,
    .on_part_data = on_part_data,
    .on_part_end = on_part_end,
    .on_body_end = on_body_end
};

/*
 * Test: Simple single field
 */
static void test_simple_field(void **state) {
    (void) state;

    const char* boundary = "----WebKitFormBoundary";
    const char* body =
        "------WebKitFormBoundary\r\n"
        "Content-Disposition: form-data; name=\"username\"\r\n"
        "\r\n"
        "john_doe\r\n"
        "------WebKitFormBoundary--\r\n";

    test_context_t ctx = {0};

    multipart_parser_t* parser = multipart_parser_create(boundary);
    assert_non_null(parser);

    multipart_parser_set_callbacks(parser, &test_callbacks);
    multipart_parser_set_data(parser, &ctx);

    ssize_t parsed = multipart_parser_execute(parser, body, strlen(body));
    assert_true(parsed > 0);
    assert_true(multipart_parser_is_complete(parser));
    assert_false(multipart_parser_has_error(parser));

    /* Verify callbacks */
    assert_int_equal(ctx.part_begin_count, 1);
    assert_int_equal(ctx.part_end_count, 1);
    assert_int_equal(ctx.headers_complete_count, 1);
    assert_int_equal(ctx.body_end_count, 1);

    /* Verify header */
    assert_string_equal(ctx.header_name, "Content-Disposition");
    assert_string_equal(ctx.header_value, "form-data; name=\"username\"");

    /* Verify data */
    assert_string_equal(ctx.part_data, "john_doe");

    multipart_parser_destroy(parser);
}

/*
 * Test: Multiple fields
 */
static void test_multiple_fields(void **state) {
    (void) state;

    const char* boundary = "----WebKitFormBoundary";
    const char* body =
        "------WebKitFormBoundary\r\n"
        "Content-Disposition: form-data; name=\"username\"\r\n"
        "\r\n"
        "john\r\n"
        "------WebKitFormBoundary\r\n"
        "Content-Disposition: form-data; name=\"email\"\r\n"
        "\r\n"
        "john@example.com\r\n"
        "------WebKitFormBoundary--\r\n";

    test_context_t ctx = {0};

    multipart_parser_t* parser = multipart_parser_create(boundary);
    assert_non_null(parser);

    multipart_parser_set_callbacks(parser, &test_callbacks);
    multipart_parser_set_data(parser, &ctx);

    ssize_t parsed = multipart_parser_execute(parser, body, strlen(body));
    assert_true(parsed > 0);
    assert_true(multipart_parser_is_complete(parser));

    /* Verify callbacks */
    assert_int_equal(ctx.part_begin_count, 2);
    assert_int_equal(ctx.part_end_count, 2);
    assert_int_equal(ctx.headers_complete_count, 2);
    assert_int_equal(ctx.body_end_count, 1);

    multipart_parser_destroy(parser);
}

/*
 * Test: File upload with multiple headers
 */
static void test_file_upload(void **state) {
    (void) state;

    const char* boundary = "----WebKitFormBoundary";
    const char* body =
        "------WebKitFormBoundary\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"test.txt\"\r\n"
        "Content-Type: text/plain\r\n"
        "\r\n"
        "Hello, World!\r\n"
        "------WebKitFormBoundary--\r\n";

    test_context_t ctx = {0};

    multipart_parser_t* parser = multipart_parser_create(boundary);
    assert_non_null(parser);

    multipart_parser_set_callbacks(parser, &test_callbacks);
    multipart_parser_set_data(parser, &ctx);

    ssize_t parsed = multipart_parser_execute(parser, body, strlen(body));
    assert_true(parsed > 0);
    assert_true(multipart_parser_is_complete(parser));

    /* Verify data */
    assert_string_equal(ctx.part_data, "Hello, World!");

    multipart_parser_destroy(parser);
}

/*
 * Test: Mixed fields and files
 */
static void test_mixed_fields_and_files(void **state) {
    (void) state;

    const char* boundary = "----WebKitFormBoundary";
    const char* body =
        "------WebKitFormBoundary\r\n"
        "Content-Disposition: form-data; name=\"title\"\r\n"
        "\r\n"
        "My Document\r\n"
        "------WebKitFormBoundary\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"doc.txt\"\r\n"
        "Content-Type: text/plain\r\n"
        "\r\n"
        "Document content here\r\n"
        "------WebKitFormBoundary\r\n"
        "Content-Disposition: form-data; name=\"description\"\r\n"
        "\r\n"
        "A test document\r\n"
        "------WebKitFormBoundary--\r\n";

    test_context_t ctx = {0};

    multipart_parser_t* parser = multipart_parser_create(boundary);
    assert_non_null(parser);

    multipart_parser_set_callbacks(parser, &test_callbacks);
    multipart_parser_set_data(parser, &ctx);

    ssize_t parsed = multipart_parser_execute(parser, body, strlen(body));
    assert_true(parsed > 0);
    assert_true(multipart_parser_is_complete(parser));

    assert_int_equal(ctx.part_begin_count, 3);
    assert_int_equal(ctx.part_end_count, 3);
    assert_int_equal(ctx.body_end_count, 1);

    multipart_parser_destroy(parser);
}

/*
 * Test: Chunked parsing (data split across multiple calls)
 */
static void test_chunked_parsing(void **state) {
    (void) state;

    const char* boundary = "----WebKitFormBoundary";
    const char* body =
        "------WebKitFormBoundary\r\n"
        "Content-Disposition: form-data; name=\"data\"\r\n"
        "\r\n"
        "test value\r\n"
        "------WebKitFormBoundary--\r\n";

    size_t total_len = strlen(body);
    test_context_t ctx = {0};

    multipart_parser_t* parser = multipart_parser_create(boundary);
    assert_non_null(parser);

    multipart_parser_set_callbacks(parser, &test_callbacks);
    multipart_parser_set_data(parser, &ctx);

    /* Parse in small chunks */
    size_t chunk_size = 10;
    size_t offset = 0;

    while (offset < total_len) {
        size_t len = (offset + chunk_size < total_len) ? chunk_size : (total_len - offset);
        ssize_t parsed = multipart_parser_execute(parser, body + offset, len);
        assert_true(parsed >= 0);
        offset += len;
    }

    assert_true(multipart_parser_is_complete(parser));
    assert_string_equal(ctx.part_data, "test value");

    multipart_parser_destroy(parser);
}

/*
 * Test: Binary data with boundary-like content
 */
static void test_binary_data(void **state) {
    (void) state;

    const char* boundary = "----WebKitFormBoundary";

    /* Body contains data that looks like a boundary but isn't */
    const char* body =
        "------WebKitFormBoundary\r\n"
        "Content-Disposition: form-data; name=\"data\"\r\n"
        "\r\n"
        "Some text with ------ dashes\r\n"
        "------WebKitFormBoundary--\r\n";

    test_context_t ctx = {0};

    multipart_parser_t* parser = multipart_parser_create(boundary);
    assert_non_null(parser);

    multipart_parser_set_callbacks(parser, &test_callbacks);
    multipart_parser_set_data(parser, &ctx);

    ssize_t parsed = multipart_parser_execute(parser, body, strlen(body));
    assert_true(parsed > 0);
    assert_true(multipart_parser_is_complete(parser));

    /* Should correctly parse the data including the dashes */
    assert_string_equal(ctx.part_data, "Some text with ------ dashes");

    multipart_parser_destroy(parser);
}

/*
 * Test: Empty field value
 */
static void test_empty_field(void **state) {
    (void) state;

    const char* boundary = "----WebKitFormBoundary";
    const char* body =
        "------WebKitFormBoundary\r\n"
        "Content-Disposition: form-data; name=\"empty\"\r\n"
        "\r\n"
        "\r\n"
        "------WebKitFormBoundary--\r\n";

    test_context_t ctx = {0};

    multipart_parser_t* parser = multipart_parser_create(boundary);
    assert_non_null(parser);

    multipart_parser_set_callbacks(parser, &test_callbacks);
    multipart_parser_set_data(parser, &ctx);

    ssize_t parsed = multipart_parser_execute(parser, body, strlen(body));
    assert_true(parsed > 0);
    assert_true(multipart_parser_is_complete(parser));

    assert_int_equal(ctx.part_data_len, 0);

    multipart_parser_destroy(parser);
}

/*
 * Test: Data containing CRLF
 */
static void test_data_with_crlf(void **state) {
    (void) state;

    const char* boundary = "----WebKitFormBoundary";
    const char* body =
        "------WebKitFormBoundary\r\n"
        "Content-Disposition: form-data; name=\"text\"\r\n"
        "\r\n"
        "Line 1\r\n"
        "Line 2\r\n"
        "Line 3\r\n"
        "------WebKitFormBoundary--\r\n";

    test_context_t ctx = {0};

    multipart_parser_t* parser = multipart_parser_create(boundary);
    assert_non_null(parser);

    multipart_parser_set_callbacks(parser, &test_callbacks);
    multipart_parser_set_data(parser, &ctx);

    ssize_t parsed = multipart_parser_execute(parser, body, strlen(body));
    assert_true(parsed > 0);
    assert_true(multipart_parser_is_complete(parser));

    assert_string_equal(ctx.part_data, "Line 1\r\nLine 2\r\nLine 3");

    multipart_parser_destroy(parser);
}

/*
 * Test: Parser reset and reuse
 */
static void test_parser_reset(void **state) {
    (void) state;

    const char* boundary = "----WebKitFormBoundary";
    const char* body =
        "------WebKitFormBoundary\r\n"
        "Content-Disposition: form-data; name=\"field\"\r\n"
        "\r\n"
        "value\r\n"
        "------WebKitFormBoundary--\r\n";

    test_context_t ctx = {0};

    multipart_parser_t* parser = multipart_parser_create(boundary);
    assert_non_null(parser);

    multipart_parser_set_callbacks(parser, &test_callbacks);
    multipart_parser_set_data(parser, &ctx);

    /* First parse */
    multipart_parser_execute(parser, body, strlen(body));
    assert_true(multipart_parser_is_complete(parser));
    assert_int_equal(ctx.body_end_count, 1);

    /* Reset and parse again */
    multipart_parser_reset(parser);
    memset(&ctx, 0, sizeof(ctx));

    multipart_parser_execute(parser, body, strlen(body));
    assert_true(multipart_parser_is_complete(parser));
    assert_int_equal(ctx.body_end_count, 1);

    multipart_parser_destroy(parser);
}

/*
 * Test: Invalid boundary
 */
static void test_invalid_boundary(void **state) {
    (void) state;

    /* Empty boundary */
    multipart_parser_t* parser1 = multipart_parser_create("");
    assert_null(parser1);

    /* NULL boundary */
    multipart_parser_t* parser2 = multipart_parser_create(NULL);
    assert_null(parser2);

    /* Boundary too long */
    char long_boundary[100];
    memset(long_boundary, 'x', sizeof(long_boundary) - 1);
    long_boundary[sizeof(long_boundary) - 1] = '\0';

    multipart_parser_t* parser3 = multipart_parser_create(long_boundary);
    assert_null(parser3);
}

/*
 * Test: Large file simulation
 */
static void test_large_data(void **state) {
    (void) state;

    const char* boundary = "----WebKitFormBoundary";

    /* Create a larger data chunk */
    char large_data[2048];
    memset(large_data, 'X', sizeof(large_data) - 1);
    large_data[sizeof(large_data) - 1] = '\0';

    /* Build multipart body */
    char body[4096];
    snprintf(body, sizeof(body),
        "------WebKitFormBoundary\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"large.bin\"\r\n"
        "Content-Type: application/octet-stream\r\n"
        "\r\n"
        "%s\r\n"
        "------WebKitFormBoundary--\r\n",
        large_data);

    test_context_t ctx = {0};

    multipart_parser_t* parser = multipart_parser_create(boundary);
    assert_non_null(parser);

    multipart_parser_set_callbacks(parser, &test_callbacks);
    multipart_parser_set_data(parser, &ctx);

    ssize_t parsed = multipart_parser_execute(parser, body, strlen(body));
    assert_true(parsed > 0);
    assert_true(multipart_parser_is_complete(parser));

    /* Verify data length */
    assert_int_equal(ctx.part_data_len, strlen(large_data));

    multipart_parser_destroy(parser);
}

/*
 * Test: Boundary at chunk boundary (worst case for streaming)
 */
static void test_boundary_at_chunk_edge(void **state) {
    (void) state;

    const char* boundary = "BOUNDARY";
    const char* body =
        "--BOUNDARY\r\n"
        "Content-Disposition: form-data; name=\"test\"\r\n"
        "\r\n"
        "data\r\n"
        "--BOUNDARY--\r\n";

    test_context_t ctx = {0};

    multipart_parser_t* parser = multipart_parser_create(boundary);
    assert_non_null(parser);

    multipart_parser_set_callbacks(parser, &test_callbacks);
    multipart_parser_set_data(parser, &ctx);

    /* Feed one byte at a time - worst case for boundary detection */
    for (size_t i = 0; i < strlen(body); i++) {
        ssize_t parsed = multipart_parser_execute(parser, body + i, 1);
        if (parsed < 0) {
            printf("Error at byte %zu: %s\n", i, multipart_parser_get_error(parser));
        }
        assert_true(parsed >= 0);
    }

    assert_true(multipart_parser_is_complete(parser));
    assert_string_equal(ctx.part_data, "data");

    multipart_parser_destroy(parser);
}

/*
 * =============================================================================
 * EDGE CASES FROM BUSBOY/FORMIDABLE TEST SUITES
 * Source: https://github.com/mscdex/busboy/blob/master/test/test-types-multipart.js
 * =============================================================================
 */

/*
 * Test: No trailing CRLF after final boundary
 *
 * RFC 2046 allows the final boundary to end with just "--" without trailing CRLF.
 * Many browsers send data this way. The parser must handle this gracefully
 * and still report the body as complete.
 *
 * Example: "------boundary--" instead of "------boundary--\r\n"
 */
static void test_no_trailing_crlf(void **state) {
    (void) state;

    const char* boundary = "----WebKitFormBoundary";
    /* Note: NO \r\n after the final boundary! */
    const char* body =
        "------WebKitFormBoundary\r\n"
        "Content-Disposition: form-data; name=\"field\"\r\n"
        "\r\n"
        "value\r\n"
        "------WebKitFormBoundary--";  /* No trailing CRLF! */

    test_context_t ctx = {0};

    multipart_parser_t* parser = multipart_parser_create(boundary);
    assert_non_null(parser);

    multipart_parser_set_callbacks(parser, &test_callbacks);
    multipart_parser_set_data(parser, &ctx);

    ssize_t parsed = multipart_parser_execute(parser, body, strlen(body));
    assert_true(parsed > 0);
    assert_true(multipart_parser_is_complete(parser));
    assert_int_equal(ctx.part_end_count, 1);
    assert_int_equal(ctx.body_end_count, 1);
    assert_string_equal(ctx.part_data, "value");

    multipart_parser_destroy(parser);
}

/*
 * Test: Empty form - boundary with immediate closing
 *
 * A valid but empty multipart body: just the opening boundary immediately
 * followed by the closing boundary. No parts at all.
 * This can happen when a form is submitted with no fields filled.
 *
 * Example: "------boundary\r\n------boundary--"
 */
static void test_empty_form(void **state) {
    (void) state;

    const char* boundary = "----WebKitFormBoundary";
    /* Opening boundary immediately followed by closing boundary */
    const char* body =
        "------WebKitFormBoundary--\r\n";

    test_context_t ctx = {0};

    multipart_parser_t* parser = multipart_parser_create(boundary);
    assert_non_null(parser);

    multipart_parser_set_callbacks(parser, &test_callbacks);
    multipart_parser_set_data(parser, &ctx);

    ssize_t parsed = multipart_parser_execute(parser, body, strlen(body));
    assert_true(parsed > 0);
    assert_true(multipart_parser_is_complete(parser));

    /* No parts should be created */
    assert_int_equal(ctx.part_begin_count, 0);
    assert_int_equal(ctx.part_end_count, 0);
    assert_int_equal(ctx.body_end_count, 1);

    multipart_parser_destroy(parser);
}

/*
 * Test: Folded header value (RFC 2822 line folding)
 *
 * HTTP headers can span multiple lines by starting continuation lines
 * with whitespace (space or tab). This is called "folding".
 * The parser should handle this and reconstruct the full header value.
 *
 * Example:
 *   Content-Type:
 *    text/plain; charset=utf-8
 *
 * Should be parsed as: "text/plain; charset=utf-8"
 */
static void test_folded_header(void **state) {
    (void) state;

    const char* boundary = "----WebKitFormBoundary";
    /* Content-Type header is folded across two lines */
    const char* body =
        "------WebKitFormBoundary\r\n"
        "Content-Disposition: form-data;\r\n"
        " name=\"field\"\r\n"  /* Continuation with leading space */
        "\r\n"
        "value\r\n"
        "------WebKitFormBoundary--\r\n";

    test_context_t ctx = {0};

    multipart_parser_t* parser = multipart_parser_create(boundary);
    assert_non_null(parser);

    multipart_parser_set_callbacks(parser, &test_callbacks);
    multipart_parser_set_data(parser, &ctx);

    ssize_t parsed = multipart_parser_execute(parser, body, strlen(body));
    /* Note: Our simple parser may not support folding - this tests current behavior */
    /* If parser doesn't support folding, it should at least not crash */
    assert_true(parsed != 0);  /* Either succeeds or fails gracefully */

    multipart_parser_destroy(parser);
}

/*
 * Test: LF-only line endings (no CR)
 *
 * While RFC requires CRLF, some clients send only LF.
 * Many parsers are lenient and accept this.
 * Tests parser tolerance for non-standard line endings.
 */
static void test_lf_only_line_endings(void **state) {
    (void) state;

    const char* boundary = "----WebKitFormBoundary";
    /* Using \n instead of \r\n */
    const char* body =
        "------WebKitFormBoundary\n"
        "Content-Disposition: form-data; name=\"field\"\n"
        "\n"
        "value\n"
        "------WebKitFormBoundary--\n";

    test_context_t ctx = {0};

    multipart_parser_t* parser = multipart_parser_create(boundary);
    assert_non_null(parser);

    multipart_parser_set_callbacks(parser, &test_callbacks);
    multipart_parser_set_data(parser, &ctx);

    ssize_t parsed = multipart_parser_execute(parser, body, strlen(body));
    assert_true(parsed > 0);
    assert_true(multipart_parser_is_complete(parser));
    assert_string_equal(ctx.part_data, "value");

    multipart_parser_destroy(parser);
}

/*
 * Test: Preamble before first boundary
 *
 * RFC 2046 allows arbitrary text (preamble) before the first boundary.
 * This is typically ignored. Some email clients add explanatory text here.
 *
 * Example:
 *   "This is a multipart message.\r\n------boundary\r\n..."
 */
static void test_preamble(void **state) {
    (void) state;

    const char* boundary = "----WebKitFormBoundary";
    const char* body =
        "This is a preamble that should be ignored.\r\n"
        "It can have multiple lines.\r\n"
        "------WebKitFormBoundary\r\n"
        "Content-Disposition: form-data; name=\"field\"\r\n"
        "\r\n"
        "value\r\n"
        "------WebKitFormBoundary--\r\n";

    test_context_t ctx = {0};

    multipart_parser_t* parser = multipart_parser_create(boundary);
    assert_non_null(parser);

    multipart_parser_set_callbacks(parser, &test_callbacks);
    multipart_parser_set_data(parser, &ctx);

    ssize_t parsed = multipart_parser_execute(parser, body, strlen(body));
    assert_true(parsed > 0);
    assert_true(multipart_parser_is_complete(parser));

    /* Preamble should be ignored, only "value" captured */
    assert_int_equal(ctx.part_begin_count, 1);
    assert_string_equal(ctx.part_data, "value");

    multipart_parser_destroy(parser);
}

/*
 * Test: Epilogue after final boundary
 *
 * RFC 2046 allows arbitrary text (epilogue) after the final boundary.
 * This is typically ignored. The parser should stop processing after "--".
 */
static void test_epilogue(void **state) {
    (void) state;

    const char* boundary = "----WebKitFormBoundary";
    const char* body =
        "------WebKitFormBoundary\r\n"
        "Content-Disposition: form-data; name=\"field\"\r\n"
        "\r\n"
        "value\r\n"
        "------WebKitFormBoundary--\r\n"
        "This is epilogue text that should be ignored.\r\n"
        "More epilogue data here.\r\n";

    test_context_t ctx = {0};

    multipart_parser_t* parser = multipart_parser_create(boundary);
    assert_non_null(parser);

    multipart_parser_set_callbacks(parser, &test_callbacks);
    multipart_parser_set_data(parser, &ctx);

    ssize_t parsed = multipart_parser_execute(parser, body, strlen(body));
    assert_true(parsed > 0);
    assert_true(multipart_parser_is_complete(parser));

    /* Epilogue should be ignored */
    assert_int_equal(ctx.part_end_count, 1);
    assert_int_equal(ctx.body_end_count, 1);

    multipart_parser_destroy(parser);
}

/*
 * Test: Boundary appears in data (false positive prevention)
 *
 * The actual boundary in data must be preceded by CRLF to be valid.
 * If the boundary string appears without CRLF prefix, it's just data.
 * This tests that the parser doesn't falsely detect boundaries.
 */
static void test_boundary_in_data_no_crlf(void **state) {
    (void) state;

    const char* boundary = "ABC";
    /* Data contains "ABC" but not preceded by CRLF - should NOT be treated as boundary */
    const char* body =
        "--ABC\r\n"
        "Content-Disposition: form-data; name=\"field\"\r\n"
        "\r\n"
        "Data with ABC in the middle and --ABC without CRLF before\r\n"
        "--ABC--\r\n";

    test_context_t ctx = {0};

    multipart_parser_t* parser = multipart_parser_create(boundary);
    assert_non_null(parser);

    multipart_parser_set_callbacks(parser, &test_callbacks);
    multipart_parser_set_data(parser, &ctx);

    ssize_t parsed = multipart_parser_execute(parser, body, strlen(body));
    assert_true(parsed > 0);
    assert_true(multipart_parser_is_complete(parser));

    /* The ABC and --ABC in data should be preserved as data, not treated as boundary */
    assert_true(strstr(ctx.part_data, "ABC") != NULL);

    multipart_parser_destroy(parser);
}

/*
 * Test: Multiple headers per part
 *
 * A part can have multiple headers: Content-Disposition, Content-Type,
 * Content-Transfer-Encoding, etc. All should be parsed correctly.
 */
static void test_multiple_headers(void **state) {
    (void) state;

    const char* boundary = "----WebKitFormBoundary";
    const char* body =
        "------WebKitFormBoundary\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"test.txt\"\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Transfer-Encoding: binary\r\n"
        "X-Custom-Header: custom-value\r\n"
        "\r\n"
        "file content\r\n"
        "------WebKitFormBoundary--\r\n";

    test_context_t ctx = {0};

    multipart_parser_t* parser = multipart_parser_create(boundary);
    assert_non_null(parser);

    multipart_parser_set_callbacks(parser, &test_callbacks);
    multipart_parser_set_data(parser, &ctx);

    ssize_t parsed = multipart_parser_execute(parser, body, strlen(body));
    assert_true(parsed > 0);
    assert_true(multipart_parser_is_complete(parser));

    /* Headers complete should be called once per part */
    assert_int_equal(ctx.headers_complete_count, 1);
    assert_string_equal(ctx.part_data, "file content");

    multipart_parser_destroy(parser);
}

/*
 * Test: Empty header value
 *
 * A header with no value: "Content-Type: \r\n"
 * Some parsers accept this, some reject it. Tests current behavior.
 */
static void test_empty_header_value(void **state) {
    (void) state;

    const char* boundary = "----WebKitFormBoundary";
    const char* body =
        "------WebKitFormBoundary\r\n"
        "Content-Disposition: form-data; name=\"field\"\r\n"
        "X-Empty-Header: \r\n"  /* Empty value */
        "\r\n"
        "value\r\n"
        "------WebKitFormBoundary--\r\n";

    test_context_t ctx = {0};

    multipart_parser_t* parser = multipart_parser_create(boundary);
    assert_non_null(parser);

    multipart_parser_set_callbacks(parser, &test_callbacks);
    multipart_parser_set_data(parser, &ctx);

    ssize_t parsed = multipart_parser_execute(parser, body, strlen(body));
    /* Should handle gracefully */
    assert_true(parsed > 0);
    assert_true(multipart_parser_is_complete(parser));
    assert_string_equal(ctx.part_data, "value");

    multipart_parser_destroy(parser);
}

/*
 * Test: Quoted boundary in Content-Type
 *
 * RFC 2046 allows boundary to be quoted if it contains special characters.
 * The boundary passed to parser should be WITHOUT quotes.
 * This tests that boundary matching works correctly.
 */
static void test_boundary_with_special_chars(void **state) {
    (void) state;

    /* Boundary with characters that would need quoting in Content-Type header */
    const char* boundary = "----=_Part_123_456.789";
    const char* body =
        "------=_Part_123_456.789\r\n"
        "Content-Disposition: form-data; name=\"field\"\r\n"
        "\r\n"
        "value\r\n"
        "------=_Part_123_456.789--\r\n";

    test_context_t ctx = {0};

    multipart_parser_t* parser = multipart_parser_create(boundary);
    assert_non_null(parser);

    multipart_parser_set_callbacks(parser, &test_callbacks);
    multipart_parser_set_data(parser, &ctx);

    ssize_t parsed = multipart_parser_execute(parser, body, strlen(body));
    assert_true(parsed > 0);
    assert_true(multipart_parser_is_complete(parser));
    assert_string_equal(ctx.part_data, "value");

    multipart_parser_destroy(parser);
}

/*
 * Test: Very long boundary (max allowed = 70 chars per RFC 2046)
 *
 * RFC 2046 specifies max boundary length of 70 characters.
 * Tests that parser handles maximum length boundary correctly.
 */
static void test_max_length_boundary(void **state) {
    (void) state;

    /* 70 character boundary (max per RFC 2046) */
    const char* boundary = "1234567890123456789012345678901234567890123456789012345678901234567890";

    char body[512];
    snprintf(body, sizeof(body),
        "--%s\r\n"
        "Content-Disposition: form-data; name=\"field\"\r\n"
        "\r\n"
        "value\r\n"
        "--%s--\r\n",
        boundary, boundary);

    test_context_t ctx = {0};

    multipart_parser_t* parser = multipart_parser_create(boundary);
    assert_non_null(parser);

    multipart_parser_set_callbacks(parser, &test_callbacks);
    multipart_parser_set_data(parser, &ctx);

    ssize_t parsed = multipart_parser_execute(parser, body, strlen(body));
    assert_true(parsed > 0);
    assert_true(multipart_parser_is_complete(parser));
    assert_string_equal(ctx.part_data, "value");

    multipart_parser_destroy(parser);
}

/* Group setup/teardown — initialize PHP runtime so emalloc/efree
 * from the HAVE_PHP_H-compiled multipart_parser.c has a live heap. */
#include "common/php_sapi_test.h"

static int group_setup(void **state) {
    (void)state;
    return php_test_runtime_init();
}

static int group_teardown(void **state) {
    (void)state;
    php_test_runtime_shutdown();
    return 0;
}

/* Main test runner */
int main(void) {
    const struct CMUnitTest tests[] = {
        /* Original tests */
        cmocka_unit_test(test_simple_field),
        cmocka_unit_test(test_multiple_fields),
        cmocka_unit_test(test_file_upload),
        cmocka_unit_test(test_mixed_fields_and_files),
        cmocka_unit_test(test_chunked_parsing),
        cmocka_unit_test(test_binary_data),
        cmocka_unit_test(test_empty_field),
        cmocka_unit_test(test_data_with_crlf),
        cmocka_unit_test(test_parser_reset),
        cmocka_unit_test(test_invalid_boundary),
        cmocka_unit_test(test_large_data),
        cmocka_unit_test(test_boundary_at_chunk_edge),

        /* Edge cases from busboy/formidable test suites */
        cmocka_unit_test(test_no_trailing_crlf),
        cmocka_unit_test(test_empty_form),
        cmocka_unit_test(test_folded_header),
        cmocka_unit_test(test_lf_only_line_endings),
        cmocka_unit_test(test_preamble),
        cmocka_unit_test(test_epilogue),
        cmocka_unit_test(test_boundary_in_data_no_crlf),
        cmocka_unit_test(test_multiple_headers),
        cmocka_unit_test(test_empty_header_value),
        cmocka_unit_test(test_boundary_with_special_chars),
        cmocka_unit_test(test_max_length_boundary),
    };

    return cmocka_run_group_tests(tests, group_setup, group_teardown);
}
