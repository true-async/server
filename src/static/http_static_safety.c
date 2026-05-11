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
#include <unistd.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <errno.h>
#include <string.h>

static bool verify_path_owner_chain(const http_static_handler_t *mount, const char *fs_path);

static int open_for_policy(const http_static_handler_t *mount, const char *path)
{
	int flags = O_RDONLY | O_CLOEXEC;

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

/* REJECT pre-flight check. The sync fallback open() uses O_NOFOLLOW
 * (kernel rejects symlinks on the final component); the async hard-zero
 * path goes through ZEND_ASYNC_FS_OPEN which doesn't expose that flag.
 * resolved_under_root() catches symlinks pointing outside the mount but
 * NOT inside-mount-to-inside-mount links, leaving a hole in REJECT
 * semantics on the hot path. lstat the final component here to close
 * it. Cost: one syscall per request, only on cache miss; cache hits
 * skip this entirely (entry was already validated). */
bool http_static_symlink_policy_admits(const http_static_handler_t *mount, const char *fs_path)
{
	if (mount->flags & HTTP_STATIC_FLAG_SYMLINKS_REJECT) {
		struct stat ls;

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
	while (*seg == '/') {
		seg++;
	}

	while (*seg != '\0') {
		const char *next = strchr(seg, '/');
		const size_t seg_len = (next != NULL) ? (size_t)(next - seg) : strlen(seg);

		/* "+2" — '/' separator + NUL. */
		if (UNEXPECTED(len + 1 + seg_len + 1 > sizeof(buf))) {
			return false;
		}

		buf[len++] = '/';
		memcpy(buf + len, seg, seg_len);
		len += seg_len;
		buf[len] = '\0';

		struct stat ls;

		if (UNEXPECTED(lstat(buf, &ls) != 0)) {
			return false;
		}

		if (S_ISLNK(ls.st_mode)) {
			struct stat ts;

			if (UNEXPECTED(stat(buf, &ts) != 0)) {
				return false;
			}

			if (ls.st_uid != ts.st_uid) {
				return false;
			}
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
	return tail == '\0' || tail == '/';
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
									int *out_fd, struct stat *st)
{
	const int fd = open_for_policy(mount, path);

	if (fd < 0) {
		return false;
	}

	if (UNEXPECTED(fstat(fd, st) != 0)) {
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

	struct stat path_st;

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

	*out_fd = fd;
	return true;
}
