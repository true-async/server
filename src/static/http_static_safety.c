/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "php.h"
#include "static/http_static_safety.h"

#include <fcntl.h>
#ifndef PHP_WIN32
# include <unistd.h>
# include <sys/param.h>
#endif
#include <sys/stat.h>
#include <errno.h>
#include <string.h>

static bool verify_path_owner_chain(const http_static_handler_t *mount, const char *fs_path);

static int open_for_policy(const http_static_handler_t *mount, const char *path)
{
	int flags = O_RDONLY | O_CLOEXEC;
#ifdef O_BINARY
	flags |= O_BINARY;
#endif

#ifdef O_NOFOLLOW
	/* REJECT: kernel-level — any symlink on the final component fails
	 * with ELOOP. Intermediate components are still followed by
	 * open(2); resolved_under_root() catches escapes via realpath.
	 *
	 * OWNER: pre-flight verify_path_owner_chain runs an explicit
	 * lstat/stat sweep across every segment and bans owner-mismatched
	 * symlinks. We DO want open() to follow links here (the sweep
	 * already approved them), so no O_NOFOLLOW. */
	if (mount->flags & HTTP_STATIC_FLAG_SYMLINKS_REJECT) {
		flags |= O_NOFOLLOW;
	}
#endif

	return open(path, flags);
}

/* REJECT pre-flight check. Both open() paths — the sync fallback
 * (open_for_policy) and the send_file engine — pass O_NOFOLLOW so the
 * kernel rejects a final-component symlink atomically. This lstat
 * rejects it earlier still, before the engine machinery spins up, and
 * covers inside-mount-to-inside-mount links that resolved_under_root()
 * (realpath prefix) cannot see. Cost: one syscall per request, only on
 * cache miss; cache hits skip this entirely (entry already validated). */
bool http_static_symlink_policy_admits(const http_static_handler_t *mount, const char *fs_path)
{
	if (mount->flags & HTTP_STATIC_FLAG_SYMLINKS_REJECT) {
		zend_stat_t ls;

		if (UNEXPECTED(lstat(fs_path, &ls) != 0)) {
			return false;
		}

		if (S_ISLNK(ls.st_mode)) {
			return false;
		}

		return true;
	}

	if (mount->flags & HTTP_STATIC_FLAG_SYMLINKS_OWNER) {
		return verify_path_owner_chain(mount, fs_path);
	}

	/* FOLLOW (default fallthrough): no symlink-specific policy. */
	return true;
}

/* OWNER_MATCH — walk the resolved path one
 * segment at a time from mount root to the final component. For each
 * segment that is a symlink, lstat() yields the link's uid, stat()
 * yields the target's uid; mismatched uids fail the policy.
 *
 * Called only when the mount runs in OWNER mode. Cost is one lstat per
 * segment plus one stat per symlink; for typical 2-4 segment paths
 * that's a handful of microseconds on warm dentry. The open-file cache
 * piggybacks on this validation: an entry was inserted only after this
 * sweep approved the path within the same TTL window, so cache hits
 * skip the sweep too.
 *
 * Returns true on accept. Stops at the first denying segment. */
static bool verify_path_owner_chain(const http_static_handler_t *mount, const char *fs_path)
{
	const size_t root_len = ZSTR_LEN(mount->root_directory);

	if (UNEXPECTED(strncmp(fs_path, ZSTR_VAL(mount->root_directory), root_len) != 0)) {
		return false;
	}

	char buf[MAXPATHLEN];
	size_t len = root_len;

	if (UNEXPECTED(root_len >= sizeof(buf))) {
		return false;
	}

	memcpy(buf, ZSTR_VAL(mount->root_directory), root_len);
	buf[len] = '\0';

	const char *seg = fs_path + root_len;
	while (*seg == '/'
#ifdef PHP_WIN32
		|| *seg == '\\'
#endif
	) {
		seg++;
	}

	while (*seg != '\0') {
#ifdef PHP_WIN32
		const char *next_fwd = strchr(seg, '/');
		const char *next_bwd = strchr(seg, '\\');
		const char *next = (next_fwd && next_bwd) ? (next_fwd < next_bwd ? next_fwd : next_bwd)
		                  : (next_fwd ? next_fwd : next_bwd);
#else
		const char *next = strchr(seg, '/');
#endif
		const size_t seg_len = (next != NULL) ? (size_t)(next - seg) : strlen(seg);

		/* "+2" — '/' separator + NUL. */
		if (UNEXPECTED(len + 1 + seg_len + 1 > sizeof(buf))) {
			return false;
		}

#ifdef PHP_WIN32
		buf[len++] = '\\';
#else
		buf[len++] = '/';
#endif
		memcpy(buf + len, seg, seg_len);
		len += seg_len;
		buf[len] = '\0';

		zend_stat_t ls;

		if (UNEXPECTED(lstat(buf, &ls) != 0)) {
			return false;
		}

		if (S_ISLNK(ls.st_mode)) {
#ifndef PHP_WIN32
			/* On Windows: lstat() maps to stat() and S_ISLNK is always 0,
			 * so this branch is a dead no-op; st_uid is always 0 anyway. */
			zend_stat_t ts;

			if (UNEXPECTED(stat(buf, &ts) != 0)) {
				return false;
			}

			if (ls.st_uid != ts.st_uid) {
				return false;
			}
#endif
		}

		if (next == NULL) {
			break;
		}

		seg = next + 1;
	}

	return true;
}

/* After try_open_candidate succeeds we know the FINAL component is
 * not a symlink (O_NOFOLLOW handles that). Intermediate components
 * are still followed by open(2), so a symlink at any level inside
 * the mount root could redirect us outside. realpath()-based prefix
 * verification closes that gap. The TOCTOU between realpath() and
 * the open we already did is acceptable — exploiting it requires
 * filesystem write access on the host. */
bool http_static_resolved_under_root(const http_static_handler_t *mount, const char *path)
{
	char canonical[MAXPATHLEN];

	if (UNEXPECTED(realpath(path, canonical) == NULL)) {
		return false;
	}

	const char *root = ZSTR_VAL(mount->root_directory);
	const size_t root_len = ZSTR_LEN(mount->root_directory);

	if (strncmp(canonical, root, root_len) != 0) {
		return false;
	}

	/* canonical == root exactly, or canonical[root_len] is a separator
	 * (subpath). Otherwise canonical only happens to share a prefix
	 * (e.g. root="/srv/foo", canonical="/srv/foobar/x"). */
	const char tail = canonical[root_len];
#ifdef PHP_WIN32
	return tail == '\0' || tail == '/' || tail == '\\';
#else
	return tail == '\0' || tail == '/';
#endif
}

/* Try open + fstat. On a non-regular file, surface ENOENT so the
 * caller's fallthrough mirrors the missing-file path uniformly.
 *
 * §13d TOCTOU retrofit: between symlink_policy_admits()'s lstat-walk
 * and open() here, an attacker with write access on an intermediate
 * directory could swap a name. We catch that by re-stat'ing the path
 * after open and comparing dev/ino against fstat(fd). A swap that
 * lands on a different inode is rejected with EPERM. This is the
 * cheapest portable defense; the openat-chain rewrite remains
 * future work. */
bool http_static_try_open_candidate(const http_static_handler_t *mount, const char *path,
									int *out_fd, zend_stat_t *st)
{
	const int fd = open_for_policy(mount, path);

	if (fd < 0) {
		return false;
	}

	if (UNEXPECTED(php_sys_fstat(fd, st) != 0)) {
		const int saved_errno = errno;
		close(fd);
		errno = saved_errno;
		return false;
	}

	if (UNEXPECTED(!S_ISREG(st->st_mode))) {
		close(fd);
		errno = ENOENT;
		return false;
	}

#ifndef PHP_WIN32
	/* TOCTOU §13d: re-stat after open, compare dev/ino to detect swap attacks.
	 * Skipped on Windows: MSVC stat() always returns st_dev=0, st_ino=0
	 * so the comparison would be meaningless and could false-positive. */
	zend_stat_t path_st;

	if (UNEXPECTED(lstat(path, &path_st) != 0)) {
		const int saved_errno = errno;
		close(fd);
		errno = saved_errno;
		return false;
	}

	if (UNEXPECTED(path_st.st_dev != st->st_dev || path_st.st_ino != st->st_ino)) {
		close(fd);
		errno = EPERM;
		return false;
	}
#endif

	*out_fd = fd;
	return true;
}
