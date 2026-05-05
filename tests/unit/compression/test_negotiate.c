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

#include "compression/http_compression_negotiate.h"

/* Convenience: parse a literal C string and inspect the result. */
#define PARSE(lit, ae) do { \
    http_accept_encoding_parse((lit), sizeof(lit) - 1, &(ae)); \
} while (0)

/* ---- Accept-Encoding parser ------------------------------------------ */

static void test_default_is_identity_only(void **state)
{
    (void)state;
    /* No header sent → conservative default: only identity. The init
     * helper deliberately diverges from RFC 9110's "any coding
     * acceptable" — see helper docstring for the rationale. */
    http_accept_encoding_t ae;
    http_accept_encoding_init_default(&ae);
    assert_false(ae.gzip_acceptable);
    assert_true(ae.identity_acceptable);
    assert_int_equal(http_accept_encoding_select(&ae), HTTP_CODEC_IDENTITY);
}

static void test_empty_header_means_identity_only(void **state)
{
    (void)state;
    http_accept_encoding_t ae;
    /* RFC: empty header value → no content coding wanted. */
    PARSE("", ae);
    assert_false(ae.gzip_acceptable);
    assert_true(ae.identity_acceptable);
    assert_int_equal(http_accept_encoding_select(&ae), HTTP_CODEC_IDENTITY);
}

static void test_explicit_gzip(void **state)
{
    (void)state;
    http_accept_encoding_t ae;
    PARSE("gzip", ae);
    assert_true(ae.gzip_acceptable);
    /* Identity unseen, star unseen → identity stays acceptable by default. */
    assert_true(ae.identity_acceptable);
    assert_int_equal(http_accept_encoding_select(&ae), HTTP_CODEC_GZIP);
}

static void test_gzip_q_zero(void **state)
{
    (void)state;
    http_accept_encoding_t ae;
    PARSE("gzip;q=0", ae);
    assert_false(ae.gzip_acceptable);
    assert_true(ae.identity_acceptable);
    assert_int_equal(http_accept_encoding_select(&ae), HTTP_CODEC_IDENTITY);
}

static void test_gzip_q_zero_with_decimals(void **state)
{
    (void)state;
    http_accept_encoding_t ae;
    PARSE("gzip;q=0.000", ae);
    assert_false(ae.gzip_acceptable);
    PARSE("gzip;q=0.0", ae);
    assert_false(ae.gzip_acceptable);
}

static void test_gzip_q_almost_zero_is_acceptable(void **state)
{
    (void)state;
    http_accept_encoding_t ae;
    /* q=0.001 is non-zero; treat as accepted. */
    PARSE("gzip;q=0.001", ae);
    assert_true(ae.gzip_acceptable);
}

static void test_identity_q_zero(void **state)
{
    (void)state;
    http_accept_encoding_t ae;
    PARSE("gzip, identity;q=0", ae);
    assert_true(ae.gzip_acceptable);
    assert_false(ae.identity_acceptable);
    assert_int_equal(http_accept_encoding_select(&ae), HTTP_CODEC_GZIP);
}

static void test_star_enables_gzip(void **state)
{
    (void)state;
    http_accept_encoding_t ae;
    PARSE("*", ae);
    assert_true(ae.gzip_acceptable);
    assert_true(ae.identity_acceptable);
}

static void test_star_q_zero_excludes_identity(void **state)
{
    (void)state;
    http_accept_encoding_t ae;
    /* RFC: *;q=0 without an identity entry excludes identity too. */
    PARSE("*;q=0", ae);
    assert_false(ae.gzip_acceptable);
    assert_false(ae.identity_acceptable);
    assert_int_equal(http_accept_encoding_select(&ae), HTTP_CODEC__COUNT);
}

static void test_star_q_zero_but_identity_kept(void **state)
{
    (void)state;
    http_accept_encoding_t ae;
    PARSE("*;q=0, identity", ae);
    assert_false(ae.gzip_acceptable);
    assert_true(ae.identity_acceptable);
    assert_int_equal(http_accept_encoding_select(&ae), HTTP_CODEC_IDENTITY);
}

static void test_explicit_overrides_star(void **state)
{
    (void)state;
    http_accept_encoding_t ae;
    /* gzip explicitly rejected even though * accepts everything. */
    PARSE("*, gzip;q=0", ae);
    assert_false(ae.gzip_acceptable);
    assert_true(ae.identity_acceptable);
}

static void test_unknown_codings_ignored(void **state)
{
    (void)state;
    http_accept_encoding_t ae;
    PARSE("br, zstd, gzip", ae);
    assert_true(ae.gzip_acceptable);
}

static void test_case_insensitive_coding_and_q(void **state)
{
    (void)state;
    http_accept_encoding_t ae;
    PARSE("GZIP;Q=0.5", ae);
    assert_true(ae.gzip_acceptable);
}

static void test_lots_of_whitespace(void **state)
{
    (void)state;
    http_accept_encoding_t ae;
    PARSE("   \t gzip  ;  q=0.9  ,   identity  ;  q=0   ", ae);
    assert_true(ae.gzip_acceptable);
    assert_false(ae.identity_acceptable);
}

static void test_extra_params_ignored(void **state)
{
    (void)state;
    http_accept_encoding_t ae;
    /* Accept-ext params after q= are legal and we should not let them
     * confuse the q-zero check. */
    PARSE("gzip;q=0.8;something=else", ae);
    assert_true(ae.gzip_acceptable);
}

static void test_malformed_q_treated_as_one(void **state)
{
    (void)state;
    http_accept_encoding_t ae;
    PARSE("gzip;q=banana", ae);
    assert_true(ae.gzip_acceptable);
}

/* ---- MIME normaliser -------------------------------------------------- */

static void test_mime_normalize_simple(void **state)
{
    (void)state;
    char buf[64];
    size_t n = http_compression_mime_normalize("text/html", 9, buf, sizeof(buf));
    assert_int_equal(n, 9);
    assert_memory_equal(buf, "text/html", 9);
}

static void test_mime_normalize_strip_params(void **state)
{
    (void)state;
    char buf[64];
    size_t n = http_compression_mime_normalize(
        "  Application/JSON ; charset=utf-8 ", 35, buf, sizeof(buf));
    assert_int_equal(n, 16);
    assert_memory_equal(buf, "application/json", 16);
}

static void test_mime_normalize_only_params(void **state)
{
    (void)state;
    char buf[64];
    size_t n = http_compression_mime_normalize("; charset=utf-8", 15, buf, sizeof(buf));
    assert_int_equal(n, 0);
}

static void test_mime_normalize_buffer_too_small(void **state)
{
    (void)state;
    char buf[4];
    size_t n = http_compression_mime_normalize("text/html", 9, buf, sizeof(buf));
    assert_int_equal(n, 0);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_default_is_identity_only),
        cmocka_unit_test(test_empty_header_means_identity_only),
        cmocka_unit_test(test_explicit_gzip),
        cmocka_unit_test(test_gzip_q_zero),
        cmocka_unit_test(test_gzip_q_zero_with_decimals),
        cmocka_unit_test(test_gzip_q_almost_zero_is_acceptable),
        cmocka_unit_test(test_identity_q_zero),
        cmocka_unit_test(test_star_enables_gzip),
        cmocka_unit_test(test_star_q_zero_excludes_identity),
        cmocka_unit_test(test_star_q_zero_but_identity_kept),
        cmocka_unit_test(test_explicit_overrides_star),
        cmocka_unit_test(test_unknown_codings_ignored),
        cmocka_unit_test(test_case_insensitive_coding_and_q),
        cmocka_unit_test(test_lots_of_whitespace),
        cmocka_unit_test(test_extra_params_ignored),
        cmocka_unit_test(test_malformed_q_treated_as_one),
        cmocka_unit_test(test_mime_normalize_simple),
        cmocka_unit_test(test_mime_normalize_strip_params),
        cmocka_unit_test(test_mime_normalize_only_params),
        cmocka_unit_test(test_mime_normalize_buffer_too_small),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
