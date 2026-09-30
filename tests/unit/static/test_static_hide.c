/* +----------------------------------------------------------------------+
   | Copyright (c) TrueAsync                                              |
   +----------------------------------------------------------------------+
   | Licensed under the Apache License, Version 2.0                       |
   +----------------------------------------------------------------------+

   StaticHandler::hide() as a rule over its two inputs.

   The rule decides whether one glob covers one mount-relative path, and it has
   two halves that a mount cannot show side by side: a pattern naming no
   directory covers that file name at any depth, one naming a directory stays
   anchored at the mount root. The first half is what an operator writing
   "*.php" means, and getting it wrong costs a disclosure rather than a 404 —
   the pattern covers index.php and hands admin/tools.php to the client as
   source.

   Held here rather than through a mount because http_static_hide_glob_matches
   takes the two strings, so every row is a call. */

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include "php.h"
#include "common/php_sapi_test.h"
#include "static/static_handler.h"
#include "static/http_static_path.h"

#include <string.h>
#include <time.h>
#ifndef _WIN32
#include <fnmatch.h>
#endif

static void test_bare_pattern_covers_every_depth(void **state)
{
	(void)state;
	assert_true(http_static_hide_glob_matches("*.php", "index.php"));
	assert_true(http_static_hide_glob_matches("*.php", "admin/tools.php"));
	assert_true(http_static_hide_glob_matches("*.php", "a/b/c/deep.php"));
}

static void test_bare_pattern_reads_the_name_not_the_path(void **state)
{
	(void)state;
	/* The directory a file sits in is not part of what a bare pattern reads, so
	 * a directory named like the pattern does not drag its contents in. */
	assert_false(http_static_hide_glob_matches("*.php", "app.php/readme.txt"));
	assert_false(http_static_hide_glob_matches("secret", "secret/file.txt"));
	assert_true(http_static_hide_glob_matches("secret", "deep/secret"));
}

static void test_rooted_pattern_stays_at_the_root(void **state)
{
	(void)state;
	assert_true(http_static_hide_glob_matches("cache/*", "cache/x.txt"));
	assert_false(http_static_hide_glob_matches("cache/*", "var/cache/x.txt"));
	assert_false(http_static_hide_glob_matches("cache/*", "cache/deep/x.txt"));
}

static void test_leading_separator_pins_the_root(void **state)
{
	(void)state;
	assert_true(http_static_hide_glob_matches("/index.php", "index.php"));
	assert_false(http_static_hide_glob_matches("/index.php", "sub/index.php"));
	assert_true(http_static_hide_glob_matches("/*.php", "index.php"));
	assert_false(http_static_hide_glob_matches("/*.php", "admin/tools.php"));
}

static void test_trailing_separator_names_a_directory(void **state)
{
	(void)state;
	assert_true(http_static_hide_glob_matches("cache/", "cache/x.txt"));
	assert_true(http_static_hide_glob_matches("cache/", "cache/deep/x.txt"));
	assert_true(http_static_hide_glob_matches("cache/", "var/cache/x.txt"));
	assert_true(http_static_hide_glob_matches("cache/", "a/b/cache/deep/x.txt"));
	/* The directory itself, when a request names it. */
	assert_true(http_static_hide_glob_matches("cache/", "var/cache"));
	assert_false(http_static_hide_glob_matches("cache/", "var/cached/x.txt"));
	assert_false(http_static_hide_glob_matches("cache/", "cache.txt"));

	/* A separator inside anchors it, as it does everywhere else. */
	assert_true(http_static_hide_glob_matches("var/cache/", "var/cache/x.txt"));
	assert_false(http_static_hide_glob_matches("var/cache/", "app/var/cache/x.txt"));
}

static void test_double_star_crosses_separators(void **state)
{
	(void)state;
	assert_true(http_static_hide_glob_matches("cache/**", "cache/x.txt"));
	assert_true(http_static_hide_glob_matches("cache/**", "cache/deep/x.txt"));
	assert_false(http_static_hide_glob_matches("cache/**", "var/cache/x.txt"));
	assert_true(http_static_hide_glob_matches("**/secret.txt", "a/b/secret.txt"));
	/* Every directory includes the one the pattern is written against, and the
	 * separator after the stars has nothing to match there. */
	assert_true(http_static_hide_glob_matches("**/secret.txt", "secret.txt"));
	assert_false(http_static_hide_glob_matches("**/secret.txt", "secret.txt.bak"));

	/* A run of stars says what one says, and costs what one costs. */
	assert_true(http_static_hide_glob_matches("logs/****", "logs/deep/app.log"));
}

static void test_pattern_covers_nothing_it_does_not_name(void **state)
{
	(void)state;
	assert_false(http_static_hide_glob_matches("*.php", "app.svg"));
	assert_false(http_static_hide_glob_matches("*.php", "assets/app.js"));
	assert_false(http_static_hide_glob_matches(NULL, "index.php"));
	assert_false(http_static_hide_glob_matches("*.php", NULL));
	/* A separator and nothing else names no file. */
	assert_false(http_static_hide_glob_matches("/", "index.php"));
}

#ifndef _WIN32
/* The Windows matcher against the POSIX one, over what the hide rule feeds it:
 * lower-case names, '*' and '?', with and without PATHNAME. Brackets and
 * escapes are left out, since the Windows matcher reads them as literals. */
static void test_windows_matcher_agrees_with_fnmatch(void **state)
{
	(void)state;
	static const char *const patterns[] = {
		"*", "**", "*.php", "a*", "*a", "a*b", "a?c", "?", "*/*", "a/*", "*/a/*",
		"a*/b", "a/**", "**/b", "*a*a*b", "a*a*a*b/*", "*.tar.gz", "a/b/c", "",
	};
	static const char *const strings[] = {
		"", "a", "ab", "abc", "a/b", "a/b/c", "x/a/y", "index.php", "admin/tools.php",
		"aab", "aaab/c", "a.tar.gz", "a/a/b", "ax/b", "ax/y/b", "b", "a/", "/a",
	};

	for (size_t i = 0; i < sizeof(patterns) / sizeof(*patterns); i++) {
		for (size_t j = 0; j < sizeof(strings) / sizeof(*strings); j++) {
			for (int pathname = 0; pathname <= 1; pathname++) {
				const int want = fnmatch(patterns[i], strings[j], pathname ? FNM_PATHNAME : 0) == 0;
				const int got = http_static_glob_match(patterns[i], strings[j],
					pathname ? HTTP_STATIC_GLOB_PATHNAME : 0) == 0;

				if (want != got) {
					fail_msg("'%s' against '%s'%s: fnmatch %d, matcher %d", patterns[i], strings[j],
						pathname ? " (PATHNAME)" : "", want, got);
				}
			}
		}
	}
}
#endif

/* The shape that took the recursive matcher 10 s at 512 bytes and past 30 s at
 * 2 KiB: many stars over a repeating literal that finally does not match. The
 * bound is loose on purpose; the old form does not finish inside it. */
static void test_windows_matcher_is_not_exponential(void **state)
{
	(void)state;
	char path[2 + 4096 + 1];

	memcpy(path, "x/", 2);
	memset(path + 2, 'a', 4096);
	path[2 + 4096] = '\0';

	struct timespec start, end;
	clock_gettime(CLOCK_MONOTONIC, &start);
	const int rc = http_static_glob_match("*/a*a*a*a*a*a*a*b/*", path, 0);
	clock_gettime(CLOCK_MONOTONIC, &end);

	const double seconds = (double)(end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9;

	assert_int_equal(rc, 1);
	assert_true(seconds < 1.0);
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_bare_pattern_covers_every_depth),
		cmocka_unit_test(test_bare_pattern_reads_the_name_not_the_path),
		cmocka_unit_test(test_rooted_pattern_stays_at_the_root),
		cmocka_unit_test(test_leading_separator_pins_the_root),
		cmocka_unit_test(test_trailing_separator_names_a_directory),
		cmocka_unit_test(test_double_star_crosses_separators),
		cmocka_unit_test(test_pattern_covers_nothing_it_does_not_name),
#ifndef _WIN32
		cmocka_unit_test(test_windows_matcher_agrees_with_fnmatch),
#endif
		cmocka_unit_test(test_windows_matcher_is_not_exponential),
	};

	return cmocka_run_group_tests(tests, NULL, NULL);
}
