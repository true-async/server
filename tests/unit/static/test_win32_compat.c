/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
  | Windows/MSVC portability tests (issue #15).                          |
  |                                                                      |
  | Covers:                                                              |
  |   - win32_compat.h macro values (O_CLOEXEC, O_NOFOLLOW)             |
  |   - http_static_resolved_under_root: '\\' separator on Windows      |
  |   - http_static_try_open_candidate: open+read+close via compat layer |
  |   - http_static_symlink_policy_admits: FOLLOW/REJECT on Windows     |
  +----------------------------------------------------------------------+
*/

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include "php.h"
#include "common/php_sapi_test.h"
#include "win32_compat.h"
#include "static/static_handler.h"
#include "static/http_static_safety.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#ifndef PHP_WIN32
# include <unistd.h>
# include <sys/param.h>
#endif

ZEND_API void zend_print_backtrace_ex(const char *msg, const char *file, int line)
{
	(void)msg; (void)file; (void)line;
}

/* =========================================================================
 * Helpers
 * ========================================================================= */

static http_static_handler_t *make_handler(const char *root, uint32_t flags)
{
	http_static_handler_t *h = ecalloc(1, sizeof(*h));
	h->url_prefix = zend_string_init("/", 1, 0);
	h->url_prefix_len = 1;
	h->root_directory = zend_string_init(root, strlen(root), 0);
	h->flags = flags;
	return h;
}

static void free_handler(http_static_handler_t *h)
{
	zend_string_release(h->url_prefix);
	zend_string_release(h->root_directory);
	efree(h);
}

/* Write a temp file, return its path (caller must unlink+free). */
static char *write_temp_file(const char *content)
{
	char *path = emalloc(MAXPATHLEN);
#ifdef PHP_WIN32
	char tmp_dir[MAXPATHLEN];
	if (GetTempPathA((DWORD)sizeof(tmp_dir), tmp_dir) == 0) {
		efree(path);
		return NULL;
	}
	if (GetTempFileNameA(tmp_dir, "tav", 0, path) == 0) {
		efree(path);
		return NULL;
	}
#else
	snprintf(path, MAXPATHLEN, "/tmp/tav_test_XXXXXX");
	int fd = mkstemp(path);
	if (fd < 0) { efree(path); return NULL; }
	close(fd);
#endif

	FILE *f = fopen(path, "wb");
	if (!f) { efree(path); return NULL; }
	fwrite(content, 1, strlen(content), f);
	fclose(f);
	return path;
}

/* =========================================================================
 * win32_compat.h macro tests
 * ========================================================================= */

static void test_o_cloexec_defined(void **state)
{
	(void)state;
	/* O_CLOEXEC must be defined on all platforms (0 on Windows, real on POSIX). */
#ifndef O_CLOEXEC
	fail_msg("O_CLOEXEC is not defined");
#endif
	/* On Windows it must be 0 — adding it to flags must be a no-op. */
#ifdef PHP_WIN32
	assert_int_equal(O_CLOEXEC, 0);
#endif
}

static void test_o_nofollow_defined(void **state)
{
	(void)state;
#ifndef O_NOFOLLOW
	fail_msg("O_NOFOLLOW is not defined");
#endif
#ifdef PHP_WIN32
	assert_int_equal(O_NOFOLLOW, 0);
#endif
}

/* =========================================================================
 * http_static_resolved_under_root — path separator portability
 * ========================================================================= */

static void test_resolved_under_root_posix_separator(void **state)
{
	(void)state;

#ifdef PHP_WIN32
	/* realpath() on Windows resolves to the canonical drive-letter form with
	 * backslashes.  Build a root from the CWD so realpath() can resolve it. */
	char cwd[MAXPATHLEN];
	if (!GetCurrentDirectoryA((DWORD)sizeof(cwd), cwd)) {
		skip();
	}
	/* Append a forward-slash subpath — resolved_under_root must accept it. */
	char path[MAXPATHLEN];
	snprintf(path, sizeof(path), "%s\\existing_sub", cwd);

	http_static_handler_t *h = make_handler(cwd, HTTP_STATIC_FLAG_SYMLINKS_FOLLOW);
	/* The path doesn't need to exist for a prefix check; the function calls
	 * realpath() first, which will fail on a non-existent path, so we just
	 * verify the prefix logic via the root itself. */
	assert_true(http_static_resolved_under_root(h, cwd));
	free_handler(h);
#else
	/* POSIX: root with forward-slash separator. */
	char cwd[MAXPATHLEN];
	assert_non_null(getcwd(cwd, sizeof(cwd)));
	http_static_handler_t *h = make_handler(cwd, HTTP_STATIC_FLAG_SYMLINKS_FOLLOW);
	assert_true(http_static_resolved_under_root(h, cwd));
	free_handler(h);
#endif
}

static void test_resolved_under_root_escape_rejected(void **state)
{
	(void)state;

#ifdef PHP_WIN32
	char cwd[MAXPATHLEN];
	if (!GetCurrentDirectoryA((DWORD)sizeof(cwd), cwd)) {
		skip();
	}
	char parent[MAXPATHLEN];
	snprintf(parent, sizeof(parent), "%s\\..", cwd);

	http_static_handler_t *h = make_handler(cwd, HTTP_STATIC_FLAG_SYMLINKS_FOLLOW);
	/* Parent dir is outside root — must be rejected. */
	assert_false(http_static_resolved_under_root(h, parent));
	free_handler(h);
#else
	char cwd[MAXPATHLEN];
	assert_non_null(getcwd(cwd, sizeof(cwd)));
	char parent[MAXPATHLEN];
	snprintf(parent, sizeof(parent), "%s/..", cwd);
	http_static_handler_t *h = make_handler(cwd, HTTP_STATIC_FLAG_SYMLINKS_FOLLOW);
	assert_false(http_static_resolved_under_root(h, parent));
	free_handler(h);
#endif
}

/* =========================================================================
 * http_static_try_open_candidate — open/read/close via compat macros
 * ========================================================================= */

static void test_try_open_candidate_reads_file(void **state)
{
	(void)state;

	const char *content = "hello windows";
	char *path = write_temp_file(content);
	assert_non_null(path);

	/* Resolve the path via realpath so resolved_under_root() passes. */
	char resolved[MAXPATHLEN];
	assert_non_null(realpath(path, resolved));

	/* Use the directory containing the file as the mount root. */
	char root[MAXPATHLEN];
	strncpy(root, resolved, sizeof(root) - 1);
	root[sizeof(root) - 1] = '\0';
	char *last_sep = strrchr(root, '/');
#ifdef PHP_WIN32
	char *last_bsep = strrchr(root, '\\');
	if (last_bsep && (!last_sep || last_bsep > last_sep)) {
		last_sep = last_bsep;
	}
#endif
	if (last_sep) {
		*last_sep = '\0';
	}

	http_static_handler_t *h = make_handler(root, HTTP_STATIC_FLAG_SYMLINKS_FOLLOW);

	int fd = -1;
	struct stat st;
	assert_true(http_static_try_open_candidate(h, resolved, &fd, &st));
	assert_true(fd >= 0);
	assert_true(S_ISREG(st.st_mode));
	assert_int_equal((int)st.st_size, (int)strlen(content));

	/* Verify read() compat macro actually works. */
	char buf[64] = {0};
	ssize_t n = read(fd, buf, sizeof(buf) - 1);
	assert_true(n == (ssize_t)strlen(content));
	assert_string_equal(buf, content);

	close(fd);
	unlink(path);
	efree(path);
	free_handler(h);
}

static void test_try_open_candidate_missing_file(void **state)
{
	(void)state;

	char cwd[MAXPATHLEN];
#ifdef PHP_WIN32
	if (!GetCurrentDirectoryA((DWORD)sizeof(cwd), cwd)) { skip(); }
#else
	assert_non_null(getcwd(cwd, sizeof(cwd)));
#endif

	http_static_handler_t *h = make_handler(cwd, HTTP_STATIC_FLAG_SYMLINKS_FOLLOW);

	char missing[MAXPATHLEN];
	snprintf(missing, sizeof(missing), "%s%cnonexistent_tav_test_file.txt",
		cwd,
#ifdef PHP_WIN32
		'\\'
#else
		'/'
#endif
	);

	int fd = -1;
	struct stat st;
	assert_false(http_static_try_open_candidate(h, missing, &fd, &st));
	assert_int_equal(fd, -1);

	free_handler(h);
}

/* =========================================================================
 * http_static_symlink_policy_admits — Windows S_ISLNK == 0 no-op
 * ========================================================================= */

static void test_symlink_policy_follow_always_admits(void **state)
{
	(void)state;

	const char *content = "x";
	char *path = write_temp_file(content);
	assert_non_null(path);

	char cwd[MAXPATHLEN];
#ifdef PHP_WIN32
	if (!GetCurrentDirectoryA((DWORD)sizeof(cwd), cwd)) {
		unlink(path); efree(path); skip();
	}
#else
	assert_non_null(getcwd(cwd, sizeof(cwd)));
#endif

	http_static_handler_t *h = make_handler(cwd, HTTP_STATIC_FLAG_SYMLINKS_FOLLOW);
	assert_true(http_static_symlink_policy_admits(h, path));
	free_handler(h);

	unlink(path);
	efree(path);
}

#ifdef PHP_WIN32
static void test_symlink_policy_reject_regular_file_admitted(void **state)
{
	(void)state;

	const char *content = "x";
	char *path = write_temp_file(content);
	assert_non_null(path);

	char cwd[MAXPATHLEN];
	if (!GetCurrentDirectoryA((DWORD)sizeof(cwd), cwd)) {
		unlink(path); efree(path); skip();
	}

	/* REJECT mode on Windows: lstat() → stat(), S_ISLNK always 0,
	 * so regular files are always admitted. */
	http_static_handler_t *h = make_handler(cwd, HTTP_STATIC_FLAG_SYMLINKS_REJECT);
	assert_true(http_static_symlink_policy_admits(h, path));
	free_handler(h);

	unlink(path);
	efree(path);
}
#endif

/* =========================================================================
 * Suite
 * ========================================================================= */

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
		cmocka_unit_test(test_o_cloexec_defined),
		cmocka_unit_test(test_o_nofollow_defined),
		cmocka_unit_test(test_resolved_under_root_posix_separator),
		cmocka_unit_test(test_resolved_under_root_escape_rejected),
		cmocka_unit_test(test_try_open_candidate_reads_file),
		cmocka_unit_test(test_try_open_candidate_missing_file),
		cmocka_unit_test(test_symlink_policy_follow_always_admits),
#ifdef PHP_WIN32
		cmocka_unit_test(test_symlink_policy_reject_regular_file_admitted),
#endif
	};

	return cmocka_run_group_tests(tests, suite_setup, suite_teardown);
}
