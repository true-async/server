/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

/* Static-handler dispatch — protocol-agnostic. Resolves the request
 * URL against the configured mounts, opens the file (sync or async via
 * ZEND_ASYNC_FS_OPEN depending on the path), populates `response_obj`
 * with status + headers + (for short error bodies) inline body, and
 * delegates body delivery to the protocol's send_static_response op
 * (see http_response_stream_ops_t in php_http_server.h).
 *
 * The protocol layer (HTTP/1, HTTP/2, HTTP/3) supplies dispatch
 * callbacks via http_static_dispatch_cbs_t — counters / refcount
 * pinning at HARD_ZERO arm time, finalize at on_static_done, spawn-
 * the-PHP-handler-coroutine for on_missing:Next rollback, keep-alive
 * verdict for the Connection header. Nothing in this module touches
 * http_connection_t / http1_request_ctx_t / http2_stream_t directly —
 * the protocol-specific lifecycle lives entirely behind those four
 * callbacks. */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "php.h"
#include "zend_smart_str.h"
#include "Zend/zend_async_API.h"
#include "php_http_server.h"
#include "http1/http_parser.h" /* http_request_t */
#include "static/static_handler.h"
#include "http_mime.h"
#include "static/http_static_path.h"
#include "http_etag.h"
#include "http_date.h"
#include "http_conditional.h"
#include "http_range.h"
#include "fs_util.h"
#include "send_file.h"
#include "static/http_static_cache.h"
#include "compression/http_compression_negotiate.h"
#include "compression/http_encoder.h"
#include "http_response_internal.h"

#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <string.h>

/* Per-mount overrides win — setMimeType() is documented as
 * "override the Content-Type for files with this extension". On miss
 * fall through to the project-wide builtin table. */
static bool mount_resolve_content_type(const http_static_handler_t *mount, const char *path,
									   size_t path_len, const char **out, size_t *out_len)
{
	if (mount != NULL && mount->mime_overrides != NULL) {
		char ext[32];
		const size_t ext_len = http_mime_extract_lowered_ext(path, path_len, ext, sizeof(ext));
		if (ext_len > 0) {
			const zval *override = zend_hash_str_find(mount->mime_overrides, ext, ext_len);
			if (override != NULL && Z_TYPE_P(override) == IS_STRING) {
				*out = Z_STRVAL_P(override);
				*out_len = Z_STRLEN_P(override);
				return true;
			}
		}
	}

	return http_mime_lookup_by_ext(path, path_len, out, out_len);
}

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

static bool verify_path_owner_chain(const http_static_handler_t *mount, const char *fs_path);

/* REJECT pre-flight check. The sync fallback open() uses O_NOFOLLOW
 * (kernel rejects symlinks on the final component); the async hard-zero
 * path goes through ZEND_ASYNC_FS_OPEN which doesn't expose that flag.
 * resolved_under_root() catches symlinks pointing outside the mount but
 * NOT inside-mount-to-inside-mount links, leaving a hole in REJECT
 * semantics on the hot path. lstat the final component here to close
 * it. Cost: one syscall per request, only on cache miss; cache hits
 * skip this entirely (entry was already validated). */
static bool symlink_policy_admits(const http_static_handler_t *mount, const char *fs_path)
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
static bool resolved_under_root(const http_static_handler_t *mount, const char *path)
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
static bool try_open_candidate(const http_static_handler_t *mount, const char *path, int *out_fd,
							   struct stat *st)
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

static inline bool path_targets_directory(const char *relative, const size_t relative_len)
{
	return relative_len == 0 || relative[relative_len - 1] == '/';
}

/* Sync-fallback only — push the mount's Cache-Control + extra headers
 * onto response_obj. include_content_headers=false on the 304 path
 * (RFC 9110 §15.4.5 bars Content-* on Not Modified). The engine
 * applies these via cfg->cache_control + cfg->extra_headers; this
 * helper covers the path where the protocol op is missing entirely
 * (H3 today) and we serve the file synchronously. */
static void apply_mount_headers(zend_object *response_obj, const http_static_handler_t *mount,
								bool include_content_headers)
{
	if (mount->cache_control != NULL) {
		http_response_static_set_header(response_obj, "cache-control", 13,
										ZSTR_VAL(mount->cache_control),
										ZSTR_LEN(mount->cache_control));
	}

	if (mount->extra_headers == NULL) {
		return;
	}

	zend_string *name;
	zval *value;
	ZEND_HASH_FOREACH_STR_KEY_VAL(mount->extra_headers, name, value)
	{
		if (name == NULL || Z_TYPE_P(value) != IS_STRING) {
			continue;
		}
		if (!include_content_headers && ZSTR_LEN(name) >= 8 &&
			strncasecmp(ZSTR_VAL(name), "content-", 8) == 0) {
			continue;
		}
		http_response_static_set_header(response_obj, ZSTR_VAL(name), ZSTR_LEN(name),
										Z_STRVAL_P(value), Z_STRLEN_P(value));
	}
	ZEND_HASH_FOREACH_END();
}

/* Precompressed sidecar selection. Picks a `.zst` /
 * `.br` / `.gz` sibling next to `fs_path` if (a) the mount opted in via
 * enablePrecompressed for that encoding, (b) the request's
 * Accept-Encoding lists it (via the existing http_compression_negotiate
 * machinery — same q-prio + identity-only-by-default behaviour as the
 * dynamic compression path), and (c) the file actually exists.
 *
 * On success: rewrites *fs_path_buf to "<original>.<suffix>", sets
 * *fs_path_len to the new length, and writes the codec token (literal
 * "gzip" / "br" / "zstd" — owned by the codec table) into
 * *out_encoding. Caller passes both into static_fsm_kick_off so the FSM serves
 * the compressed bytes but emits the original Content-Type plus
 * Content-Encoding.
 *
 * On no-match: returns false, leaves fs_path untouched. The caller
 * proceeds to serve the original file. */
static bool try_select_precompressed(const http_static_handler_t *mount, http_request_t *request,
									 char *fs_path_buf, size_t buf_cap, size_t *fs_path_len,
									 const char **out_encoding, size_t *out_encoding_len)
{
	if ((mount->flags & (HTTP_STATIC_FLAG_PRECOMP_BR | HTTP_STATIC_FLAG_PRECOMP_GZIP |
						 HTTP_STATIC_FLAG_PRECOMP_ZSTD)) == 0) {
		return false;
	}

	const zend_string *ae = http_request_find_header(request, "accept-encoding", 15);
	if (ae == NULL) {
		return false;
	}

	http_accept_encoding_t parsed;
	http_accept_encoding_parse(ZSTR_VAL(ae), ZSTR_LEN(ae), &parsed);

	/* Server preference: zstd > brotli > gzip. Mirrors the dynamic
	 * compression path's negotiate(). identity_acceptable doesn't gate
	 * us — we always have the original file as the identity fallback. */
	static const struct
	{
		uint32_t flag;
		const char *suffix;
		size_t suffix_len;
		const char *token;
		size_t token_len;
	} codecs[] = {
		{HTTP_STATIC_FLAG_PRECOMP_ZSTD, ".zst", 4, "zstd", 4},
		{HTTP_STATIC_FLAG_PRECOMP_BR, ".br", 3, "br", 2},
		{HTTP_STATIC_FLAG_PRECOMP_GZIP, ".gz", 3, "gzip", 4},
	};
	const bool acceptable[] = {
		parsed.zstd_acceptable,
		parsed.brotli_acceptable,
		parsed.gzip_acceptable,
	};

	for (size_t i = 0; i < sizeof(codecs) / sizeof(codecs[0]); i++) {
		if ((mount->flags & codecs[i].flag) == 0) {
			continue;
		}
		if (!acceptable[i]) {
			continue;
		}

		if (UNEXPECTED(*fs_path_len + codecs[i].suffix_len + 1 > buf_cap)) {
			continue;
		}
		char candidate[MAXPATHLEN];
		memcpy(candidate, fs_path_buf, *fs_path_len);
		memcpy(candidate + *fs_path_len, codecs[i].suffix, codecs[i].suffix_len);
		candidate[*fs_path_len + codecs[i].suffix_len] = '\0';

		struct stat st;
		if (stat(candidate, &st) != 0) {
			continue;
		}
		if (!S_ISREG(st.st_mode)) {
			continue;
		}

		memcpy(fs_path_buf + *fs_path_len, codecs[i].suffix, codecs[i].suffix_len);
		*fs_path_len += codecs[i].suffix_len;
		fs_path_buf[*fs_path_len] = '\0';
		*out_encoding = codecs[i].token;
		*out_encoding_len = codecs[i].token_len;
		return true;
	}
	return false;
}

http_static_result_t http_static_try_serve(http_server_object *server,
										   http_request_t *request,
										   zend_object *response_obj,
										   http_server_counters_t *counters,
										   const http_static_dispatch_cbs_t *cbs,
										   void *user)
{
	const size_t mount_count = http_static_handler_count(server);
	if (UNEXPECTED(mount_count == 0)) {
		return HTTP_STATIC_PASSTHROUGH;
	}

	if (UNEXPECTED(response_obj == NULL || request == NULL)) {
		return HTTP_STATIC_PASSTHROUGH;
	}

	/* GET/HEAD only — operators can overlay POST/PUT endpoints on the
	 * same prefix without the static layer turning them into 405s. */
	const bool is_head = http_request_method_is_head(request);
	const bool is_get = http_request_method_is_get(request);
	if (!is_get && !is_head) {
		return HTTP_STATIC_PASSTHROUGH;
	}

	/* request->path is built lazily by the PHP-side getter; req->uri is
	 * always populated by the parser. http_static_path_resolve strips
	 * '?' and '#' so the whole URI is safe to feed in. */
	const char *req_path = (request->uri != NULL) ? ZSTR_VAL(request->uri) : NULL;
	const size_t req_path_len = (request->uri != NULL) ? ZSTR_LEN(request->uri) : 0;
	if (UNEXPECTED(req_path == NULL || req_path_len == 0)) {
		return HTTP_STATIC_PASSTHROUGH;
	}

	for (size_t mi = 0; mi < mount_count; mi++) {
		const http_static_handler_t *mount = http_static_handler_get(server, mi);
		if (UNEXPECTED(mount == NULL)) {
			continue;
		}

		char fs_path[MAXPATHLEN];
		size_t fs_path_len = 0;
		const char *relative = NULL;
		size_t relative_len = 0;

		const http_static_path_result_t rc =
			http_static_path_resolve(mount, req_path, req_path_len, fs_path, sizeof(fs_path),
									 &fs_path_len, &relative, &relative_len);

		if (rc == HTTP_STATIC_PATH_NO_MATCH) {
			continue;
		}
		if (UNEXPECTED(rc == HTTP_STATIC_PATH_BAD_REQUEST)) {
			http_response_emit_status_body(response_obj, 400, "Bad Request", 11);
			return HTTP_STATIC_HANDLED;
		}
		/* Dotfile-deny / traversal escape: 404 (not 403) so existence
		 * of the restricted resource isn't disclosed. */
		if (UNEXPECTED(rc == HTTP_STATIC_PATH_FORBIDDEN)) {
			http_response_emit_status_body(response_obj, 404, "Not Found", 9);
			return HTTP_STATIC_HANDLED;
		}
		if (UNEXPECTED(rc == HTTP_STATIC_PATH_HIDE)) {
			if (mount->flags & HTTP_STATIC_FLAG_ON_MISSING_NEXT) {
				return HTTP_STATIC_PASSTHROUGH;
			}
			http_response_emit_status_body(response_obj, 404, "Not Found", 9);
			return HTTP_STATIC_HANDLED;
		}

		/* Hide-globs match against the relative path so operator-
		 * authored patterns target what they see. */
		if (UNEXPECTED(relative_len > 0 &&
					   http_static_path_is_hidden(mount, relative, relative_len))) {
			if (mount->flags & HTTP_STATIC_FLAG_ON_MISSING_NEXT) {
				return HTTP_STATIC_PASSTHROUGH;
			}
			http_response_emit_status_body(response_obj, 404, "Not Found", 9);
			return HTTP_STATIC_HANDLED;
		}

		/* Resolve directory→index synchronously before the hard-zero
		 * gate (#5b in TODO_STATIC_HANDLER_REVIEW). Cold-cache stat is
		 * a single inode lookup (microseconds); it's the read+send that
		 * we want async, and that's what hard-zero already gives us.
		 *
		 * On hit, fs_path_len is promoted to the joined path so MIME
		 * lookup sees the index file's extension (.html etc) rather
		 * than the directory's. On miss, the request resolves to 404
		 * uniformly — on_missing:Next mounts return PASSTHROUGH so the
		 * PHP handler can take over. */
		const bool was_directory = path_targets_directory(relative, relative_len);
		if (was_directory) {
			bool index_resolved = false;
			for (size_t ii = 0; ii < mount->index_count; ii++) {
				const zend_string *idx = mount->index_files[ii];
				size_t cand_len = fs_path_len;
				if (UNEXPECTED(!http_static_path_join(fs_path, sizeof(fs_path), &cand_len,
													  ZSTR_VAL(idx), ZSTR_LEN(idx)))) {
					continue;
				}
				struct stat sb;
				if (stat(fs_path, &sb) == 0 && S_ISREG(sb.st_mode)) {
					fs_path_len = cand_len;
					index_resolved = true;
					break;
				}
				/* Truncate back so the next candidate starts from the
				 * pristine directory prefix. */
				fs_path[fs_path_len] = '\0';
			}
			if (!index_resolved) {
				if (mount->flags & HTTP_STATIC_FLAG_ON_MISSING_NEXT) {
					return HTTP_STATIC_PASSTHROUGH;
				}
				http_response_emit_status_body(response_obj, 404, "Not Found", 9);
				return HTTP_STATIC_HANDLED;
			}
		}

		/* Hard-zero async path — both plain TCP and TLS rides this:
		 *  - plain TCP: kernel zero-copy via ZEND_ASYNC_IO_SENDFILE.
		 *  - TLS: chunked IO_READ → SSL_write →
		 *    cipher-drain loop driven by tls_zc_write_done_cb. No
		 *    coroutine spawned.
		 *
		 * Eligibility: resolved path must stay inside the mount root
		 * after canonicalisation. realpath here is sync but cheap on
		 * warm cache; on hot serving we skip it entirely via the
		 * open-file cache (TTL-bound, evicts oldest first).
		 *
		 * On open-error the on_missing:Next rollback in static_fsm_handle_open
		 * detaches state and hands ctx over to a regular PHP-handler
		 * coroutine, so on_missing:Next mounts also ride this path on
		 * the success path (#5c). */
		/* Precompressed sidecar: if the mount opted in via
		 * enablePrecompressed and the client lists a matching coding
		 * in Accept-Encoding, swap fs_path to the .gz/.br/.zst sibling
		 * before cache lookup. On a hit-flow this means subsequent
		 * lookups for the same compressed variant skip the sidecar
		 * stat too — the cache key is the rewritten path. The MIME
		 * type stays derived from the original (computed pre-rewrite
		 * and carried as override_ct so static_fsm_handle_stat doesn't redo
		 * the lookup against the .gz extension). */
		const char *picked_encoding = NULL;
		size_t picked_encoding_len = 0;
		const char *override_ct = NULL;
		size_t override_ct_len = 0;
		if ((mount->flags & (HTTP_STATIC_FLAG_PRECOMP_BR | HTTP_STATIC_FLAG_PRECOMP_GZIP |
							 HTTP_STATIC_FLAG_PRECOMP_ZSTD)) != 0) {
			const char *pre_ct = NULL;
			size_t pre_ct_len = 0;
			if (!mount_resolve_content_type(mount, fs_path, fs_path_len, &pre_ct, &pre_ct_len)) {
				pre_ct = "application/octet-stream";
				pre_ct_len = sizeof("application/octet-stream") - 1;
			}
			if (try_select_precompressed(mount, request, fs_path, sizeof(fs_path), &fs_path_len,
										 &picked_encoding, &picked_encoding_len)) {
				override_ct = pre_ct;
				override_ct_len = pre_ct_len;
			}
		}

		http_static_cache_t *cache = http_static_cache_acquire(server);
		bool gate_ok;
		bool have_view = false;
		http_static_cache_view_t cv;
		if (cache != NULL) {
			if (http_static_cache_lookup(cache, fs_path, fs_path_len, &cv)) {
				/* Trust-within-TTL: realpath was validated at insert,
				 * stat/etag/MIME/Last-Modified are pre-rendered. The
				 * engine skips every derivation on the cache_view path. */
				http_server_on_static_cache_hit(counters);
				gate_ok = true;
				have_view = true;
			} else {
				http_server_on_static_cache_miss(counters);
				gate_ok =
					symlink_policy_admits(mount, fs_path) && resolved_under_root(mount, fs_path);
				/* Cache insert happens inside the engine once st / etag /
				 * content_type / Last-Modified are all in hand. */
			}
		} else {
			gate_ok = symlink_policy_admits(mount, fs_path) && resolved_under_root(mount, fs_path);
		}

		/* Hand off to the single send_file engine when the protocol op
		 * is wired (H1, H2). H3 stub leaves it NULL — fall through to
		 * the synchronous slurp path below. http_static_dispatch_cbs_t
		 * is now an alias of send_file_cbs_t — pass it straight through. */
		const http_response_stream_ops_t *ops = http_response_get_stream_ops(response_obj);
		if (gate_ok && ops != NULL && ops->send_static_response != NULL) {
			send_file_config_t cfg = {0};
			cfg.abs_path = fs_path;
			cfg.abs_path_len = fs_path_len;
			cfg.etag = (mount->flags & HTTP_STATIC_FLAG_ETAG) != 0;
			cfg.last_modified = true;
			cfg.accept_ranges = true;
			cfg.conditional = true;
			cfg.cache_control = mount->cache_control;
			cfg.extra_headers = mount->extra_headers;
			cfg.mime_overrides = mount->mime_overrides;
			cfg.cache_view = have_view ? &cv : NULL;
			cfg.counters = counters;
			cfg.server = server;
			cfg.content_encoding = picked_encoding;
			cfg.content_encoding_len = picked_encoding_len;
			if (override_ct != NULL && override_ct_len > 0) {
				/* Sidecar-resolved Content-Type — synthesize a
				 * non-refcounted view so the engine reads it via the
				 * cfg.content_type slot without a full zend_string
				 * allocation. cfg lives only for the synchronous tail
				 * of send_file(); the engine copies what it needs. */
				cfg.content_type = zend_string_init(override_ct, override_ct_len, 0);
			}
			cfg.on_error = (mount->flags & HTTP_STATIC_FLAG_ON_MISSING_NEXT)
							   ? SEND_FILE_ERR_PASSTHROUGH_PHP
							   : SEND_FILE_ERR_EMIT_VIA_OP;

			const send_file_result_t r =
				send_file(request, response_obj, &cfg, cbs, user);

			if (cfg.content_type != NULL) {
				zend_string_release((zend_string *)cfg.content_type);
			}

			if (r == SEND_FILE_ASYNC) {
				/* Hard-zero telemetry — every successful engine kick-off
				 * out of the static dispatcher counts as a hard-zero hit.
				 * sendFile uses the same engine and bumps its own counter
				 * (or none) at the adapter level. */
				http_server_on_static_zero_coroutine(counters);
				return HTTP_STATIC_HARD_ZERO;
			}
			if (r == SEND_FILE_PASSTHROUGH) {
				/* engine fired on_passthrough — caller's hook already
				 * released its pinned protocol-side resources. */
				return HTTP_STATIC_PASSTHROUGH;
			}
			/* SEND_FILE_HANDLED — engine refused before kick-off (rare:
			 * MAXPATHLEN, ZEND_ASYNC_FS_OPEN failure, ecalloc failure).
			 * Fall through to the synchronous fallback path below. */
		}

		int fd = -1;
		struct stat st;
		bool opened = try_open_candidate(mount, fs_path, &fd, &st);

		if (UNEXPECTED(!opened)) {
			/* ENOENT / ELOOP (symlink rejected) / ENOTDIR / EACCES /
			 * EPERM all collapse to "not available". 404 (rather than
			 * 403 on EACCES) avoids disclosing whether a restricted
			 * file actually exists, matching the dotfile-deny path. */
			if (mount->flags & HTTP_STATIC_FLAG_ON_MISSING_NEXT) {
				return HTTP_STATIC_PASSTHROUGH;
			}
			http_response_emit_status_body(response_obj, 404, "Not Found", 9);
			return HTTP_STATIC_HANDLED;
		}

		/* Closes the intermediate-symlink-traversal gap that O_NOFOLLOW
		 * leaves open: realpath() canonicalises every segment, so a
		 * symlink anywhere on the path that points outside the mount
		 * surfaces here as a prefix mismatch. */
		if (UNEXPECTED(!resolved_under_root(mount, fs_path) ||
					   !symlink_policy_admits(mount, fs_path))) {
			close(fd);
			http_response_emit_status_body(response_obj, 404, "Not Found", 9);
			return HTTP_STATIC_HANDLED;
		}

		/* Synchronous slurp cap — protects the loop from a stray giant
		 * file. The async sendfile path will remove this limit. */
		if (UNEXPECTED((uint64_t)st.st_size > (uint64_t)HTTP_STATIC_MAX_FILE_SIZE)) {
			close(fd);
			http_response_emit_status_body(response_obj, 413, "Payload Too Large", 17);
			return HTTP_STATIC_HANDLED;
		}

		char etag_buf[HTTP_ETAG_BUF_LEN];
		const bool etag_enabled = (mount->flags & HTTP_STATIC_FLAG_ETAG) != 0;
		if (etag_enabled) {
			http_etag_format_strong(&st, etag_buf);
		}

		char last_modified_buf[HTTP_DATE_BUF_LEN];
		http_date_format_imf(st.st_mtime, last_modified_buf);

		const zend_string *if_none_match = http_request_find_header(request, "if-none-match", 13);
		const zend_string *if_modified_since =
			http_request_find_header(request, "if-modified-since", 17);
		const bool not_modified = http_conditional_check(
			if_none_match != NULL ? ZSTR_VAL(if_none_match) : NULL,
			if_none_match != NULL ? ZSTR_LEN(if_none_match) : 0,
			if_modified_since != NULL ? ZSTR_VAL(if_modified_since) : NULL,
			if_modified_since != NULL ? ZSTR_LEN(if_modified_since) : 0,
			etag_enabled ? etag_buf : NULL, etag_enabled ? HTTP_ETAG_LEN : 0, st.st_mtime);

		if (not_modified) {
			close(fd);
			http_response_static_set_status(response_obj, 304);
			if (etag_enabled) {
				http_response_static_set_header(response_obj, "etag", 4, etag_buf,
												HTTP_ETAG_LEN);
			}
			http_response_static_set_header(response_obj, "last-modified", 13, last_modified_buf,
											HTTP_DATE_LEN);
			apply_mount_headers(response_obj, mount, false);
			return HTTP_STATIC_HANDLED;
		}

		zend_string *body = NULL;
		if (is_get) {
			body = fs_slurp_fd(fd, (size_t)st.st_size);
			if (UNEXPECTED(body == NULL)) {
				close(fd);
				http_response_emit_status_body(response_obj, 500, "Internal Server Error", 21);
				return HTTP_STATIC_HANDLED;
			}
		}
		close(fd);

		http_response_static_set_status(response_obj, 200);

		const char *content_type = NULL;
		size_t content_type_len = 0;
		if (!mount_resolve_content_type(mount, fs_path, fs_path_len, &content_type,
										&content_type_len)) {
			content_type = "application/octet-stream";
			content_type_len = sizeof("application/octet-stream") - 1;
		}
		http_response_static_set_header(response_obj, "content-type", 12, content_type,
										content_type_len);

		if (etag_enabled) {
			http_response_static_set_header(response_obj, "etag", 4, etag_buf,
											HTTP_ETAG_LEN);
		}
		http_response_static_set_header(response_obj, "last-modified", 13, last_modified_buf,
										HTTP_DATE_LEN);
		apply_mount_headers(response_obj, mount, true);

		if (is_head) {
			/* The format-time path computes Content-Length from the
			 * body smart_str; with an empty body we have to advertise
			 * the would-be size explicitly. */
			http_response_set_content_length(response_obj, (uint64_t)st.st_size);
		} else {
			http_response_static_set_body_str(response_obj, body);
			zend_string_release(body);
		}

		return HTTP_STATIC_HANDLED;
	}

	return HTTP_STATIC_PASSTHROUGH;
}
