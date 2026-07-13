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
#include "static/http_static_safety.h"
#include "http_mime.h"
#include "static/http_static_path.h"
#include "http_etag.h"
#include "http_date.h"
#include "http_range.h"
#include "http_precompressed.h"
#include "send_file.h"
#include "static/http_static_cache.h"
#include "http_response_internal.h"

#include <sys/stat.h>
#include <sys/types.h>
#ifndef PHP_WIN32
# include <fcntl.h>
# include <unistd.h>
#endif
/* close() → _close() on Windows via win32_compat.h (php_http_server.h). */
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
								const bool include_content_headers)
{
	if (mount->cache_control != NULL) {
		http_response_static_set_header(response_obj, "cache-control", 13,
										ZSTR_VAL(mount->cache_control),
										ZSTR_LEN(mount->cache_control));
	}

	http_response_apply_extra_headers(response_obj, mount->extra_headers,
									  include_content_headers);
}

/* Translate mount-side enablePrecompressed bits into the codec mask
 * understood by http_precompressed_select(). */
static inline uint32_t mount_precomp_mask(const uint32_t mount_flags)
{
	uint32_t mask = 0;

	if (mount_flags & HTTP_STATIC_FLAG_PRECOMP_BR) {
		mask |= HTTP_PRECOMP_BR;
	}

	if (mount_flags & HTTP_STATIC_FLAG_PRECOMP_GZIP) {
		mask |= HTTP_PRECOMP_GZIP;
	}

	if (mount_flags & HTTP_STATIC_FLAG_PRECOMP_ZSTD) {
		mask |= HTTP_PRECOMP_ZSTD;
	}

	return mask;
}

/* Shared 200/304 header emit — both the cache-hit fast path and the
 * sync fallback funnel through these so the two never drift. The
 * cache-hit path passes cache_view fields; the sync path passes the
 * freshly formatted etag / Last-Modified stack buffers. A NULL etag or
 * last_modified slot is skipped. */
static void static_emit_validators(zend_object *response_obj, const char *etag,
								   const size_t etag_len, const char *last_modified,
								   const size_t last_modified_len)
{
	if (etag != NULL) {
		http_response_static_set_header(response_obj, "etag", 4, etag, etag_len);
	}

	if (last_modified != NULL) {
		http_response_static_set_header(response_obj, "last-modified", 13, last_modified,
										last_modified_len);
	}
}

static void static_emit_not_modified(zend_object *response_obj,
									 const http_static_handler_t *mount, const char *etag,
									 const size_t etag_len, const char *last_modified,
									 const size_t last_modified_len)
{
	http_response_static_set_status(response_obj, 304);
	static_emit_validators(response_obj, etag, etag_len, last_modified, last_modified_len);
	apply_mount_headers(response_obj, mount, false);
}

static void static_emit_ok_headers(zend_object *response_obj,
								   const http_static_handler_t *mount, const char *content_type,
								   const size_t content_type_len, const char *encoding,
								   const size_t encoding_len, const char *etag,
								   const size_t etag_len, const char *last_modified,
								   const size_t last_modified_len)
{
	http_response_static_set_status(response_obj, 200);
	http_response_static_set_header(response_obj, "content-type", 12, content_type,
									content_type_len);

	if (encoding != NULL && encoding_len > 0) {
		http_response_static_set_header(response_obj, "content-encoding", 16, encoding,
										encoding_len);
		http_response_static_set_header(response_obj, "vary", 4, "Accept-Encoding", 15);
	}

	static_emit_validators(response_obj, etag, etag_len, last_modified, last_modified_len);
	apply_mount_headers(response_obj, mount, true);
}

http_static_result_t http_static_try_serve(http_server_object *server,
										   http_request_t *request,
										   zend_object *response_obj,
										   http_server_counters_t *counters,
										   const http_static_dispatch_cbs_t *cbs,
										   void *user)
{
	return http_static_try_serve_mounts(
		server,
		http_static_handler_mounts(server), http_static_handler_count(server),
		http_static_cache_acquire(server), request, response_obj, counters, cbs,
		user);
}

http_static_result_t http_static_try_serve_mounts(
	http_server_object *server,
	const http_static_handler_t *const *mounts, size_t mount_count,
	struct http_static_cache_s *cache,
	http_request_t *request,
	zend_object *response_obj,
	http_server_counters_t *counters,
	const http_static_dispatch_cbs_t *cbs,
	void *user)
{
	if (UNEXPECTED(mount_count == 0 || mounts == NULL)) {
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
		const http_static_handler_t *mount = mounts[mi];

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

				zend_stat_t sb;

				if (VCWD_STAT(fs_path, &sb) == 0 && S_ISREG(sb.st_mode)) {
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

		const uint32_t precomp_mask = mount_precomp_mask(mount->flags);

		if (precomp_mask != 0) {
			const char *pre_ct = NULL;
			size_t pre_ct_len = 0;

			if (!mount_resolve_content_type(mount, fs_path, fs_path_len, &pre_ct, &pre_ct_len)) {
				pre_ct = "application/octet-stream";
				pre_ct_len = sizeof("application/octet-stream") - 1;
			}

			if (http_precompressed_select(request, precomp_mask, fs_path, sizeof(fs_path),
										  &fs_path_len, &picked_encoding, &picked_encoding_len,
										  cache)) {
				override_ct = pre_ct;
				override_ct_len = pre_ct_len;
			}
		}

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
					http_static_symlink_policy_admits(mount, fs_path) && http_static_resolved_under_root(mount, fs_path);
				/* Cache insert happens inside the engine once st / etag /
				 * content_type / Last-Modified are all in hand. */
			}
		} else {
			gate_ok = http_static_symlink_policy_admits(mount, fs_path) && http_static_resolved_under_root(mount, fs_path);
		}

		/* Small-file fast-path bypass. When the cache already has a
		 * validated view and the file is below the slurp threshold, the
		 * async engine's whole machinery — 0-ms timer defer, file_io
		 * handle, on_armed/on_done callbacks, FSM state, dedicated
		 * h2 head-only-with-inline branch — buys nothing. The file
		 * fits in a single synchronous read(2); the dispose+commit
		 * pipeline (skip_handler=true) is the same path a PHP handler
		 * with $response->setBody() would take, and the rest of the
		 * stack already amortises it. Fall through to the inline slurp
		 * path below, which builds the response synchronously and
		 * returns HTTP_STATIC_HANDLED. Big files (> threshold), range
		 * requests, and HEAD stay on the engine — they actually benefit
		 * from async sendfile / chunked-read overlap. */
		const bool prefer_inline = have_view
		                           && is_get
		                           && (uint64_t)cv.st.st_size <= (uint64_t)SEND_FILE_SLURP_THRESHOLD
		                           && http_request_find_header(request, "range", 5) == NULL;

		/* Hot HIT: build response from cv + cached body, skip open/fstat/close.
		 * prefer_inline implies have_view, which is only set under cache != NULL. */
		if (prefer_inline) {
			zend_string *const cached_body =
				http_static_cache_body_acquire(cache, fs_path, fs_path_len);

			if (cached_body != NULL) {
				const bool etag_enabled = (mount->flags & HTTP_STATIC_FLAG_ETAG) != 0;
				const zend_string *const if_none_match =
					http_request_find_header(request, "if-none-match", 13);
				const zend_string *const if_modified_since =
					http_request_find_header(request, "if-modified-since", 17);
				const bool not_modified = http_conditional_check(
					if_none_match != NULL ? ZSTR_VAL(if_none_match) : NULL,
					if_none_match != NULL ? ZSTR_LEN(if_none_match) : 0,
					if_modified_since != NULL ? ZSTR_VAL(if_modified_since) : NULL,
					if_modified_since != NULL ? ZSTR_LEN(if_modified_since) : 0,
					etag_enabled ? cv.etag : NULL,
					etag_enabled ? cv.etag_len : 0,
					cv.st.st_mtime);

				if (not_modified) {
					zend_string_release(cached_body);
					static_emit_not_modified(response_obj, mount,
											 etag_enabled ? cv.etag : NULL, cv.etag_len,
											 cv.last_modified, cv.last_modified_len);
					return HTTP_STATIC_HANDLED;
				}

				const char *content_type = NULL;
				size_t content_type_len = 0;

				if (override_ct != NULL && override_ct_len > 0) {
					content_type = override_ct;
					content_type_len = override_ct_len;
				} else if (cv.content_type != NULL) {
					content_type = cv.content_type;
					content_type_len = cv.content_type_len;
				} else {
					content_type = "application/octet-stream";
					content_type_len = sizeof("application/octet-stream") - 1;
				}

				static_emit_ok_headers(response_obj, mount, content_type, content_type_len,
									   picked_encoding, picked_encoding_len,
									   etag_enabled ? cv.etag : NULL, cv.etag_len,
									   cv.last_modified, cv.last_modified_len);

				http_response_static_set_body_view(response_obj, cached_body);
				zend_string_release(cached_body);
				return HTTP_STATIC_HANDLED;
			}
		}

		const http_response_stream_ops_t *ops = http_response_get_stream_ops(response_obj);

		if (!prefer_inline && gate_ok && ops != NULL && ops->send_static_response != NULL) {
			send_file_config_t cfg = {0};
			cfg.abs_path = fs_path;
			cfg.abs_path_len = fs_path_len;
			cfg.etag = (mount->flags & HTTP_STATIC_FLAG_ETAG) != 0;
			cfg.last_modified = true;
			cfg.accept_ranges = true;
			cfg.conditional = true;
			cfg.reject_symlinks = (mount->flags & HTTP_STATIC_FLAG_SYMLINKS_REJECT) != 0;
			cfg.cache_control = mount->cache_control;
			cfg.extra_headers = mount->extra_headers;
			cfg.mime_overrides = mount->mime_overrides;
			cfg.cache_view = have_view ? &cv : NULL;
			cfg.server = server;
			cfg.cache = cache;
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
		zend_stat_t st;
		bool opened = http_static_try_open_candidate(mount, fs_path, &fd, &st);

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
		if (UNEXPECTED(!http_static_resolved_under_root(mount, fs_path) ||
					   !http_static_symlink_policy_admits(mount, fs_path))) {
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
			static_emit_not_modified(response_obj, mount,
									 etag_enabled ? etag_buf : NULL, HTTP_ETAG_LEN,
									 last_modified_buf, HTTP_DATE_LEN);
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

			if (prefer_inline) {
				http_static_cache_body_store(cache, fs_path, fs_path_len, ZSTR_VAL(body),
											 ZSTR_LEN(body));
			}
		}

		close(fd);

		const char *content_type = NULL;
		size_t content_type_len = 0;

		/* Precompressed sidecar: prefer the override Content-Type that
		 * was resolved against the ORIGINAL file's extension (the .br /
		 * .gz sidecar's own extension is not in the MIME table and
		 * would fall through to application/octet-stream — and a
		 * browser that sees that without Content-Encoding will render
		 * the compressed payload as garbage). */
		if (override_ct != NULL && override_ct_len > 0) {
			content_type = override_ct;
			content_type_len = override_ct_len;
		} else if (!mount_resolve_content_type(mount, fs_path, fs_path_len, &content_type,
											   &content_type_len)) {
			content_type = "application/octet-stream";
			content_type_len = sizeof("application/octet-stream") - 1;
		}

		static_emit_ok_headers(response_obj, mount, content_type, content_type_len,
							   picked_encoding, picked_encoding_len,
							   etag_enabled ? etag_buf : NULL, HTTP_ETAG_LEN,
							   last_modified_buf, HTTP_DATE_LEN);

		if (is_head) {
			/* The format-time path computes Content-Length from the
			 * body smart_str; with an empty body we have to advertise
			 * the would-be size explicitly. */
			http_response_set_content_length(response_obj, (uint64_t)st.st_size);
		} else {
			http_response_static_set_body_view(response_obj, body);
			zend_string_release(body);
		}

		return HTTP_STATIC_HANDLED;
	}

	return HTTP_STATIC_PASSTHROUGH;
}
