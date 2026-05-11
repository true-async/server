/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
  | Unit tests for static-handler decoders (issue #13 §13c.4).           |
  | Covers:                                                              |
  |   - http_static_mime_lookup       (mime extension lookup)            |
  |   - http_static_conditional_match (If-None-Match + If-Modified-Since)|
  |   - http_static_path_resolve      (percent-decode + segment guard)   |
  +----------------------------------------------------------------------+
*/

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include "php.h"
#include "common/php_sapi_test.h"
#include "static/static_handler.h"
#include "static/http_static_mime.h"
#include "static/http_static_etag.h"
#include "static/http_static_path.h"

/* libphp is linked but does not export zend_print_backtrace_ex (used by
 * ZEND_ASSERT on debug builds). Provide a minimal stub so the test
 * binary links — assert failures would crash the test anyway. */
ZEND_API void zend_print_backtrace_ex(const char *msg, const char *file, int line)
{
	(void)msg;
	(void)file;
	(void)line;
}

/* ========================================================================
 * Helpers — build a minimal http_static_handler_t for tests.
 * ======================================================================== */

static http_static_handler_t *make_test_handler(const char *url_prefix,
												const char *root_directory, uint32_t flags)
{
	http_static_handler_t *h = ecalloc(1, sizeof(*h));
	h->url_prefix = zend_string_init(url_prefix, strlen(url_prefix), 0);
	h->url_prefix_len = ZSTR_LEN(h->url_prefix);
	h->root_directory = zend_string_init(root_directory, strlen(root_directory), 0);
	h->flags = flags;
	return h;
}

static void free_test_handler(http_static_handler_t *h)
{
	if (h->url_prefix != NULL) {
		zend_string_release(h->url_prefix);
	}
	if (h->root_directory != NULL) {
		zend_string_release(h->root_directory);
	}
	efree(h);
}

/* ========================================================================
 * MIME lookup
 * ======================================================================== */

static void test_mime_lookup_known(void **state)
{
	(void)state;
	const char *out = NULL;
	size_t out_len = 0;

	assert_true(http_static_mime_lookup(NULL, "x.html", 6, &out, &out_len));
	assert_string_equal(out, "text/html; charset=utf-8");

	assert_true(http_static_mime_lookup(NULL, "x.JS", 4, &out, &out_len));
	assert_string_equal(out, "text/javascript; charset=utf-8");

	assert_true(http_static_mime_lookup(NULL, "img.PNG", 7, &out, &out_len));
	assert_string_equal(out, "image/png");
}

static void test_mime_lookup_unknown(void **state)
{
	(void)state;
	const char *out = NULL;
	size_t out_len = 0;
	assert_false(http_static_mime_lookup(NULL, "file.unknownext", 15, &out, &out_len));
}

static void test_mime_lookup_no_extension(void **state)
{
	(void)state;
	const char *out = NULL;
	size_t out_len = 0;
	assert_false(http_static_mime_lookup(NULL, "Makefile", 8, &out, &out_len));
	assert_false(http_static_mime_lookup(NULL, "", 0, &out, &out_len));
}

static void test_mime_lookup_dotfile(void **state)
{
	(void)state;
	/* ".bashrc" — leading dot but no real extension. find_extension_offset
	 * returns offset 1 (past the dot), the rest is "bashrc" — not in the
	 * built-in table. */
	const char *out = NULL;
	size_t out_len = 0;
	assert_false(http_static_mime_lookup(NULL, ".bashrc", 7, &out, &out_len));
}

static void test_mime_lookup_separator_guard(void **state)
{
	(void)state;
	/* "foo.tar/bar" — last '.' is in the parent dir; must not leak it. */
	const char *out = NULL;
	size_t out_len = 0;
	assert_false(http_static_mime_lookup(NULL, "foo.tar/bar", 11, &out, &out_len));
}

static void test_mime_lookup_extension_overflow(void **state)
{
	(void)state;
	/* 64-byte extension > stack buf cap (32). Should reject, not crash. */
	char path[80];
	path[0] = '.';
	for (size_t i = 1; i < sizeof(path) - 1; i++) {
		path[i] = 'a';
	}
	path[sizeof(path) - 1] = '\0';
	const char *out = NULL;
	size_t out_len = 0;
	assert_false(http_static_mime_lookup(NULL, path, strlen(path), &out, &out_len));
}

static void test_mime_lookup_multiple_dots(void **state)
{
	(void)state;
	/* "archive.tar.gz" — rightmost dot wins; "gz" → application/gzip. */
	const char *out = NULL;
	size_t out_len = 0;
	assert_true(http_static_mime_lookup(NULL, "archive.tar.gz", 14, &out, &out_len));
	assert_string_equal(out, "application/gzip");
}

/* ========================================================================
 * If-None-Match / If-Modified-Since
 * ======================================================================== */

static void test_inm_single_weak_match(void **state)
{
	(void)state;
	const char *etag = "W/\"abc123\"";
	const char *header = "W/\"abc123\"";
	assert_true(
		http_static_conditional_match(header, strlen(header), NULL, 0, etag, strlen(etag), 0));
}

static void test_inm_strong_vs_weak_equal(void **state)
{
	(void)state;
	/* RFC 9110 §13.1.2: weak-equal — strip W/ on both sides. */
	const char *etag = "W/\"abc\"";
	const char *header = "\"abc\"";
	assert_true(
		http_static_conditional_match(header, strlen(header), NULL, 0, etag, strlen(etag), 0));
}

static void test_inm_wildcard(void **state)
{
	(void)state;
	const char *etag = "W/\"abc\"";
	const char *header = "*";
	assert_true(
		http_static_conditional_match(header, strlen(header), NULL, 0, etag, strlen(etag), 0));
}

static void test_inm_list_match(void **state)
{
	(void)state;
	const char *etag = "W/\"def\"";
	const char *header = "W/\"abc\", W/\"def\", W/\"ghi\"";
	assert_true(
		http_static_conditional_match(header, strlen(header), NULL, 0, etag, strlen(etag), 0));
}

static void test_inm_no_match(void **state)
{
	(void)state;
	const char *etag = "W/\"xyz\"";
	const char *header = "W/\"abc\", W/\"def\"";
	assert_false(
		http_static_conditional_match(header, strlen(header), NULL, 0, etag, strlen(etag), 0));
}

static void test_inm_malformed_double_comma(void **state)
{
	(void)state;
	/* ",,, W/\"abc\" ,,," — empty entries must not match. */
	const char *etag = "W/\"abc\"";
	const char *header = ",, W/\"abc\" ,,";
	assert_true(
		http_static_conditional_match(header, strlen(header), NULL, 0, etag, strlen(etag), 0));
}

static void test_ims_not_modified(void **state)
{
	(void)state;
	/* mtime 2020-01-01 00:00:00 UTC = 1577836800. If-Modified-Since later
	 * → not modified. */
	const char *ims = "Sat, 01 Feb 2020 00:00:00 GMT";
	assert_true(http_static_conditional_match(NULL, 0, ims, strlen(ims), NULL, 0, 1577836800));
}

static void test_ims_modified(void **state)
{
	(void)state;
	const char *ims = "Wed, 01 Jan 2020 00:00:00 GMT"; /* = 1577836800 */
	assert_false(
		http_static_conditional_match(NULL, 0, ims, strlen(ims), NULL, 0, 1577923200 /* +1d */));
}

static void test_ims_garbage_rejected(void **state)
{
	(void)state;
	const char *ims = "this is not a date";
	assert_false(http_static_conditional_match(NULL, 0, ims, strlen(ims), NULL, 0, 1577836800));
}

static void test_ims_out_of_range_rejected(void **state)
{
	(void)state;
	/* day=99 is out of range; tm_fields_in_range must reject. */
	const char *ims = "Sun, 99 Nov 1994 08:49:37 GMT";
	assert_false(http_static_conditional_match(NULL, 0, ims, strlen(ims), NULL, 0, 0));
}

static void test_ims_leap_second_rejected(void **state)
{
	(void)state;
	const char *ims = "Sun, 06 Nov 1994 08:49:60 GMT";
	assert_false(http_static_conditional_match(NULL, 0, ims, strlen(ims), NULL, 0, 0));
}

static void test_inm_takes_precedence(void **state)
{
	(void)state;
	/* When both headers present, INM wins (RFC 9110 §13.1.2). INM does
	 * not match → return false even if IMS would say not modified. */
	const char *etag = "W/\"abc\"";
	const char *inm = "W/\"def\"";
	const char *ims = "Sat, 01 Feb 2020 00:00:00 GMT";
	assert_false(http_static_conditional_match(inm, strlen(inm), ims, strlen(ims), etag,
											   strlen(etag), 1577836800));
}

/* ========================================================================
 * Path resolve — percent-decode + segment validation
 * ======================================================================== */

static void test_path_resolve_basic(void **state)
{
	(void)state;
	http_static_handler_t *h = make_test_handler("/static/", "/var/www", 0);

	char buf[4096];
	size_t out_len = 0;
	const char *rel = NULL;
	size_t rel_len = 0;

	const http_static_path_result_t rc = http_static_path_resolve(
		h, "/static/foo.txt", strlen("/static/foo.txt"), buf, sizeof(buf), &out_len, &rel, &rel_len);
	assert_int_equal(rc, HTTP_STATIC_PATH_OK);
	assert_string_equal(buf, "/var/www/foo.txt");

	free_test_handler(h);
}

static void test_path_resolve_percent_decoded(void **state)
{
	(void)state;
	http_static_handler_t *h = make_test_handler("/s/", "/r", 0);

	char buf[4096];
	size_t out_len = 0;
	const http_static_path_result_t rc = http_static_path_resolve(
		h, "/s/a%20b.txt", strlen("/s/a%20b.txt"), buf, sizeof(buf), &out_len, NULL, NULL);
	assert_int_equal(rc, HTTP_STATIC_PATH_OK);
	assert_string_equal(buf, "/r/a b.txt");

	free_test_handler(h);
}

static void test_path_resolve_nul_byte_rejected(void **state)
{
	(void)state;
	http_static_handler_t *h = make_test_handler("/s/", "/r", 0);

	char buf[4096];
	size_t out_len = 0;
	const http_static_path_result_t rc = http_static_path_resolve(
		h, "/s/a%00b", strlen("/s/a%00b"), buf, sizeof(buf), &out_len, NULL, NULL);
	assert_int_equal(rc, HTTP_STATIC_PATH_BAD_REQUEST);

	free_test_handler(h);
}

static void test_path_resolve_backslash_rejected(void **state)
{
	(void)state;
	http_static_handler_t *h = make_test_handler("/s/", "/r", 0);

	char buf[4096];
	size_t out_len = 0;

	/* literal */
	http_static_path_result_t rc = http_static_path_resolve(
		h, "/s/a\\b", strlen("/s/a\\b"), buf, sizeof(buf), &out_len, NULL, NULL);
	assert_int_equal(rc, HTTP_STATIC_PATH_BAD_REQUEST);

	/* percent-encoded */
	rc = http_static_path_resolve(h, "/s/a%5Cb", strlen("/s/a%5Cb"), buf, sizeof(buf), &out_len,
								  NULL, NULL);
	assert_int_equal(rc, HTTP_STATIC_PATH_BAD_REQUEST);

	free_test_handler(h);
}

static void test_path_resolve_dotdot_rejected(void **state)
{
	(void)state;
	http_static_handler_t *h = make_test_handler("/s/", "/r", 0);

	char buf[4096];
	size_t out_len = 0;
	http_static_path_result_t rc = http_static_path_resolve(
		h, "/s/../etc/passwd", strlen("/s/../etc/passwd"), buf, sizeof(buf), &out_len, NULL, NULL);
	assert_int_equal(rc, HTTP_STATIC_PATH_FORBIDDEN);

	rc = http_static_path_resolve(h, "/s/foo/../bar", strlen("/s/foo/../bar"), buf, sizeof(buf),
								  &out_len, NULL, NULL);
	assert_int_equal(rc, HTTP_STATIC_PATH_FORBIDDEN);

	rc = http_static_path_resolve(h, "/s/foo/%2E%2E/bar", strlen("/s/foo/%2E%2E/bar"), buf,
								  sizeof(buf), &out_len, NULL, NULL);
	assert_int_equal(rc, HTTP_STATIC_PATH_FORBIDDEN);

	free_test_handler(h);
}

static void test_path_resolve_dot_rejected(void **state)
{
	(void)state;
	http_static_handler_t *h = make_test_handler("/s/", "/r", 0);

	char buf[4096];
	size_t out_len = 0;
	const http_static_path_result_t rc = http_static_path_resolve(
		h, "/s/./foo", strlen("/s/./foo"), buf, sizeof(buf), &out_len, NULL, NULL);
	assert_int_equal(rc, HTTP_STATIC_PATH_FORBIDDEN);

	free_test_handler(h);
}

static void test_path_resolve_double_slash_rejected(void **state)
{
	(void)state;
	http_static_handler_t *h = make_test_handler("/s/", "/r", 0);

	char buf[4096];
	size_t out_len = 0;
	const http_static_path_result_t rc = http_static_path_resolve(
		h, "/s/foo//bar", strlen("/s/foo//bar"), buf, sizeof(buf), &out_len, NULL, NULL);
	assert_int_equal(rc, HTTP_STATIC_PATH_BAD_REQUEST);

	free_test_handler(h);
}

static void test_path_resolve_truncated_escape(void **state)
{
	(void)state;
	http_static_handler_t *h = make_test_handler("/s/", "/r", 0);

	char buf[4096];
	size_t out_len = 0;
	http_static_path_result_t rc = http_static_path_resolve(h, "/s/a%", strlen("/s/a%"), buf,
															sizeof(buf), &out_len, NULL, NULL);
	assert_int_equal(rc, HTTP_STATIC_PATH_BAD_REQUEST);

	rc = http_static_path_resolve(h, "/s/a%2", strlen("/s/a%2"), buf, sizeof(buf), &out_len, NULL,
								  NULL);
	assert_int_equal(rc, HTTP_STATIC_PATH_BAD_REQUEST);

	rc = http_static_path_resolve(h, "/s/a%XY", strlen("/s/a%XY"), buf, sizeof(buf), &out_len, NULL,
								  NULL);
	assert_int_equal(rc, HTTP_STATIC_PATH_BAD_REQUEST);

	free_test_handler(h);
}

static void test_path_resolve_dotfile_deny(void **state)
{
	(void)state;
	http_static_handler_t *h =
		make_test_handler("/s/", "/r", HTTP_STATIC_FLAG_DOTFILES_DENY);

	char buf[4096];
	size_t out_len = 0;
	const http_static_path_result_t rc = http_static_path_resolve(
		h, "/s/.env", strlen("/s/.env"), buf, sizeof(buf), &out_len, NULL, NULL);
	assert_int_equal(rc, HTTP_STATIC_PATH_FORBIDDEN);

	free_test_handler(h);
}

static void test_path_resolve_dotfile_ignore(void **state)
{
	(void)state;
	http_static_handler_t *h =
		make_test_handler("/s/", "/r", HTTP_STATIC_FLAG_DOTFILES_IGNORE);

	char buf[4096];
	size_t out_len = 0;
	const http_static_path_result_t rc = http_static_path_resolve(
		h, "/s/.env", strlen("/s/.env"), buf, sizeof(buf), &out_len, NULL, NULL);
	assert_int_equal(rc, HTTP_STATIC_PATH_HIDE);

	free_test_handler(h);
}

static void test_path_resolve_query_stripped(void **state)
{
	(void)state;
	http_static_handler_t *h = make_test_handler("/s/", "/r", 0);

	char buf[4096];
	size_t out_len = 0;
	const http_static_path_result_t rc = http_static_path_resolve(
		h, "/s/a.txt?x=1", strlen("/s/a.txt?x=1"), buf, sizeof(buf), &out_len, NULL, NULL);
	assert_int_equal(rc, HTTP_STATIC_PATH_OK);
	assert_string_equal(buf, "/r/a.txt");

	free_test_handler(h);
}

static void test_path_resolve_prefix_mismatch(void **state)
{
	(void)state;
	http_static_handler_t *h = make_test_handler("/s/", "/r", 0);

	char buf[4096];
	size_t out_len = 0;
	const http_static_path_result_t rc = http_static_path_resolve(
		h, "/other/foo", strlen("/other/foo"), buf, sizeof(buf), &out_len, NULL, NULL);
	assert_int_equal(rc, HTTP_STATIC_PATH_NO_MATCH);

	free_test_handler(h);
}

static void test_path_resolve_depth_cap(void **state)
{
	(void)state;
	http_static_handler_t *h = make_test_handler("/s/", "/r", 0);

	/* Build "/s/" + ("a/" * 300) — 300 > HTTP_STATIC_PATH_MAX_SEGMENTS (256). */
	char url[800];
	size_t pos = 0;
	memcpy(url + pos, "/s/", 3);
	pos += 3;
	for (int i = 0; i < 300; i++) {
		url[pos++] = 'a';
		url[pos++] = '/';
	}
	url[pos] = '\0';

	char buf[4096];
	size_t out_len = 0;
	const http_static_path_result_t rc =
		http_static_path_resolve(h, url, pos, buf, sizeof(buf), &out_len, NULL, NULL);
	assert_int_equal(rc, HTTP_STATIC_PATH_BAD_REQUEST);

	free_test_handler(h);
}

/* ========================================================================
 * Suite registration
 * ======================================================================== */

static int suite_setup(void **state)
{
	(void)state;
	return php_test_runtime_init();
}

static int suite_teardown(void **state)
{
	(void)state;
	php_test_runtime_shutdown();
	return 0;
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		/* MIME */
		cmocka_unit_test(test_mime_lookup_known),
		cmocka_unit_test(test_mime_lookup_unknown),
		cmocka_unit_test(test_mime_lookup_no_extension),
		cmocka_unit_test(test_mime_lookup_dotfile),
		cmocka_unit_test(test_mime_lookup_separator_guard),
		cmocka_unit_test(test_mime_lookup_extension_overflow),
		cmocka_unit_test(test_mime_lookup_multiple_dots),

		/* If-None-Match / If-Modified-Since */
		cmocka_unit_test(test_inm_single_weak_match),
		cmocka_unit_test(test_inm_strong_vs_weak_equal),
		cmocka_unit_test(test_inm_wildcard),
		cmocka_unit_test(test_inm_list_match),
		cmocka_unit_test(test_inm_no_match),
		cmocka_unit_test(test_inm_malformed_double_comma),
		cmocka_unit_test(test_ims_not_modified),
		cmocka_unit_test(test_ims_modified),
		cmocka_unit_test(test_ims_garbage_rejected),
		cmocka_unit_test(test_ims_out_of_range_rejected),
		cmocka_unit_test(test_ims_leap_second_rejected),
		cmocka_unit_test(test_inm_takes_precedence),

		/* Path resolve */
		cmocka_unit_test(test_path_resolve_basic),
		cmocka_unit_test(test_path_resolve_percent_decoded),
		cmocka_unit_test(test_path_resolve_nul_byte_rejected),
		cmocka_unit_test(test_path_resolve_backslash_rejected),
		cmocka_unit_test(test_path_resolve_dotdot_rejected),
		cmocka_unit_test(test_path_resolve_dot_rejected),
		cmocka_unit_test(test_path_resolve_double_slash_rejected),
		cmocka_unit_test(test_path_resolve_truncated_escape),
		cmocka_unit_test(test_path_resolve_dotfile_deny),
		cmocka_unit_test(test_path_resolve_dotfile_ignore),
		cmocka_unit_test(test_path_resolve_query_stripped),
		cmocka_unit_test(test_path_resolve_prefix_mismatch),
		cmocka_unit_test(test_path_resolve_depth_cap),
	};

	return cmocka_run_group_tests(tests, suite_setup, suite_teardown);
}
