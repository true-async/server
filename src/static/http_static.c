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
#include "static/http_static_mime.h"
#include "static/http_static_path.h"
#include "static/http_static_etag.h"
#include "static/http_static_cache.h"
#include "compression/http_compression_negotiate.h"
#include "compression/http_encoder.h"

#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <string.h>

/* Pull the streaming vtable accessor — we read response_obj's
 * stream_ops to delegate body delivery to the H1 protocol module
 * without re-exposing http_response_get_stream_ops in a public
 * header (it's an internal cross-TU contract — same pattern as
 * compression's response wrapper). */
extern const http_response_stream_ops_t *
                  http_response_get_stream_ops(zend_object *obj);
extern void      *http_response_get_stream_ctx(zend_object *obj);

static void emit_status(zend_object *response_obj, int status, const char *body_msg,
						size_t body_msg_len)
{
	http_response_static_set_status(response_obj, status);
	http_response_static_set_header(response_obj, "content-type", 12, "text/plain; charset=utf-8",
									25);
	if (body_msg != NULL && body_msg_len > 0) {
		http_response_static_set_body_cstr(response_obj, body_msg, body_msg_len);
	}
}

static inline bool method_is_get(const http_request_t *req)
{
	return req != NULL && req->method != NULL && ZSTR_LEN(req->method) == 3 &&
		   memcmp(ZSTR_VAL(req->method), "GET", 3) == 0;
}

static inline bool method_is_head(const http_request_t *req)
{
	return req != NULL && req->method != NULL && ZSTR_LEN(req->method) == 4 &&
		   memcmp(ZSTR_VAL(req->method), "HEAD", 4) == 0;
}

static const zend_string *find_first_request_header(const http_request_t *const req,
													 const char *const name, const size_t name_len)
{
	if (req == NULL || req->headers == NULL) {
		return NULL;
	}

	const zval *const zv = zend_hash_str_find(req->headers, name, name_len);
	if (zv == NULL) {
		return NULL;
	}

	if (Z_TYPE_P(zv) == IS_STRING) {
		return Z_STR_P(zv);
	}

	if (Z_TYPE_P(zv) == IS_ARRAY) {
		const zval *const first = zend_hash_index_find(Z_ARRVAL_P(zv), 0);
		if (first != NULL && Z_TYPE_P(first) == IS_STRING) {
			return Z_STR_P(first);
		}
	}

	return NULL;
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
static int parse_byte_range(const char *hdr, size_t hdr_len, uint64_t size, uint64_t *out_first,
							uint64_t *out_last);

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

/* OwnerMatch — walk the resolved path one
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

	const char *const root = ZSTR_VAL(mount->root_directory);
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

static zend_string *slurp_fd(const int fd, const size_t size)
{
	if (size == 0) {
		return ZSTR_EMPTY_ALLOC();
	}
	zend_string *const out = zend_string_alloc(size, 0);
	size_t total = 0;

	while (total < size) {
		const ssize_t n = read(fd, ZSTR_VAL(out) + total, size - total);
		if (EXPECTED(n > 0)) {
			total += (size_t)n;
			continue;
		}

		if (n == 0) {
			break; /* premature EOF */
		}

		if (errno == EINTR) {
			continue;
		}

		zend_string_release(out);
		return NULL;
	}

	if (UNEXPECTED(total != size)) {
		zend_string_release(out);
		return NULL;
	}

	ZSTR_VAL(out)[size] = '\0';
	return out;
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

/* ===== Hard-zero async path =========================================
 *
 * Resolution + open + stat live here; status/header decisions are
 * written onto response_obj; the actual bytes-on-the-wire (status
 * line, headers, sendfile or TLS chunked encrypt) is delegated to
 * the protocol's send_static_response vtable method. For HTTP/1
 * that's src/http1/http1_sendfile.c; H2/H3 leave the slot NULL and
 * we fall back to the synchronous-populate path until they grow
 * their own implementations.
 *
 * The request lifetime is owned by a callback chain rooted at
 * ZEND_ASYNC_FS_OPEN — never spawns a coroutine, never enters the
 * PHP VM. http_request_finalize closes out the chain the same way
 * the regular coroutine dispose does, so keep-alive, drain, and
 * pipelined-request resume all keep working. */

/* Single persistent callback model: one callback registered once on
 * file_io->event for the OPEN/STAT phases. Once we hand off to the
 * protocol op the persistent callback is removed (the op installs
 * its own). Spurious fires (a callback registered mid-NOTIFY can
 * re-enter the same NOTIFY iteration) are filtered by phase versus
 * expected-req identity. */
typedef enum
{
	SS_PHASE_OPEN = 0,	  /* awaiting fs_open completion (result=NULL) */
	SS_PHASE_STAT = 1,	  /* awaiting io_stat completion (result=stat req) */
	SS_PHASE_DELEGATED = 2, /* protocol op owns delivery; awaiting on_done */
	SS_PHASE_DONE = 3,
} ss_phase_t;

typedef struct
{
	/* Protocol-agnostic dispatch hooks. Callbacks fire on completion /
	 * rollback / keep-alive query. user is opaque to the static module. */
	http_static_dispatch_cbs_t cbs;
	void *user;
	bool armed; /* on_hard_zero_armed already fired — pair with on_static_done */

	/* Telemetry sink. May be NULL (offline tests). */
	http_server_counters_t *counters;

	/* Server (for cache_acquire). */
	http_server_object *server;

	/* Inbound request — used for header lookups (Range, If-*). */
	http_request_t *request;

	/* Response object — populated with status + headers + (for inline
	 * error bodies) body before delegation. */
	zend_object *response_obj;

	const http_static_handler_t *mount;

	/* Resolved on-disk path. emalloc'd in ss_kick_off, freed by
	 * ss_state_free. */
	char *fs_path;
	size_t fs_path_len;

	/* Async file io. Acquired by ZEND_ASYNC_FS_OPEN. Ownership
	 * transfers to the protocol op once we call send_static_response;
	 * we null this slot at hand-off so finalize doesn't double-dispose. */
	zend_async_io_t *file_io;

	/* Cached fstat. */
	struct stat st;

	bool is_head;

	/* State machine cursor. */
	ss_phase_t phase;

	/* Identity of the currently-pending op's req. NOTIFY may fire our
	 * cb spuriously (registration during NOTIFY iteration races) —
	 * we ignore any result that doesn't match. NULL during the OPEN
	 * phase because libuv_fs_open notifies with result=NULL. */
	zend_async_io_req_t *pending_req;

	/* Persistent cb registered once on file_io->event at kick-off,
	 * removed at hand-off (or on early-error finalize). */
	zend_async_event_callback_t *cb;

	/* Open-file cache pre-population. When the
	 * pre-flight finds a fresh entry for this path, it copies the
	 * cached metadata into the slots below and sets has_cached_meta.
	 * ss_handle_open then skips IO_STAT (state->st pre-filled);
	 * ss_handle_stat skips etag formatting / MIME lookup / IMF-date
	 * formatting (the buffers below are pre-rendered). content_type
	 * stays a borrowed pointer into the persistent MIME table — same
	 * lifetime invariant as the cache entry. */
	bool has_cached_meta;
	bool cached_etag_enabled;
	char cached_etag_buf[HTTP_STATIC_ETAG_BUF_LEN];
	char cached_lm_buf[HTTP_STATIC_DATE_BUF_LEN];
	const char *cached_content_type;
	size_t cached_content_type_len;

	/* Precompressed sidecar support. fs_path points at the sidecar
	 * (so open targets the compressed bytes); these fields carry the
	 * *original* file's content type plus the Content-Encoding token
	 * to emit. content_encoding is a static string ("gzip" / "br" /
	 * "zstd") owned by the codec table — never freed. */
	const char *override_content_type;
	size_t override_content_type_len;
	const char *content_encoding; /* NULL = identity */
	size_t content_encoding_len;

	/* Range support (RFC 9110 §14.2). is_range flips when the
	 * pre-flight accepted a `Range: bytes=A-B` header — we'll emit
	 * 206 with a sliced body. range_first / range_last are inclusive
	 * byte offsets; range_total carries the unmodified file size for
	 * the Content-Range header. */
	bool is_range;
	uint64_t range_first;
	uint64_t range_last;
	uint64_t range_total;
} ss_state_t;

typedef struct
{
	zend_async_event_callback_t base;
	ss_state_t *state;
} ss_cb_t;

static void ss_dispatch(zend_async_event_t *event, zend_async_event_callback_t *callback,
						void *result, zend_object *exception);

static void ss_cb_dispose(zend_async_event_callback_t *cb, zend_async_event_t *event)
{
	(void)event;
	efree(cb);
}

/* Single owner of state lifetime. */
static inline void ss_state_free(ss_state_t *state)
{
	if (state == NULL) {
		return;
	}
	if (state->fs_path != NULL) {
		efree(state->fs_path);
		state->fs_path = NULL;
	}
	efree(state);
}

/* Forward decl for on_done callback wired into the protocol op. */
static void ss_on_protocol_done(void *user, int status);

/* Tear down the state machine and fire the caller's on_static_done
 * callback. After that, the caller owns post-request bookkeeping
 * (counters, finalize, keep-alive) — the static module does not touch
 * conn/ctx because it has neither.
 *
 * Used on the early-error paths (open failed before we delegated,
 * stat error) AND as the tail of ss_on_protocol_done once the
 * protocol op is finished. `status` is 0 on success, non-zero abort. */
static void ss_finalize(ss_state_t *state, int status)
{
	state->phase = SS_PHASE_DONE;

	if (state->cb != NULL && state->file_io != NULL) {
		(void)state->file_io->event.del_callback(&state->file_io->event, state->cb);
		state->cb = NULL;
	}

	if (state->file_io != NULL) {
		if (state->file_io->event.dispose != NULL) {
			state->file_io->event.dispose(&state->file_io->event);
		}
		state->file_io = NULL;
	}

	const http_static_dispatch_cbs_t cbs_copy = state->cbs;
	void *const user = state->user;
	const bool armed = state->armed;
	ss_state_free(state);

	if (armed && cbs_copy.on_static_done != NULL) {
		cbs_copy.on_static_done(user, status);
	}
}

/* Format a uint64 into `out` (buffer must be ≥ 21 bytes). Manual
 * digit-by-digit beats snprintf("%" PRIu64) by ~15× on hot paths
 * (no parse-format-string overhead, no locale lookup). Returns the
 * byte count written (excluding the NUL). */
static inline size_t ss_fmt_u64(char *out, uint64_t v)
{
	/* Worst case for uint64 is 20 digits ("18446744073709551615"). */
	char tmp[20];
	size_t n = 0;
	do {
		tmp[n++] = (char)('0' + (v % 10));
		v /= 10;
	} while (v != 0);
	for (size_t i = 0; i < n; i++) {
		out[i] = tmp[n - 1 - i];
	}
	out[n] = '\0';
	return n;
}

/* Set Content-Length on response_obj. The H1 protocol op serializes
 * headers verbatim — no auto-Content-Length insertion — so we must
 * write the number ourselves whenever the response is supposed to
 * carry one. */
static void ss_set_content_length(zend_object *response_obj, uint64_t len)
{
	char buf[24];
	const size_t n = ss_fmt_u64(buf, len);
	http_response_static_set_header(response_obj, "content-length", 14, buf, n);
}

/* Set Connection header tracking the keep-alive verdict. The verdict
 * is queried via the protocol callback; H1 reads conn->keep_alive,
 * H2/H3 always returns true (multiplex transports — Connection header
 * is filtered out at submit time anyway, see http2_strategy.c's
 * response_header_allowed). */
static bool ss_keep_alive(const ss_state_t *state)
{
	if (state->cbs.keep_alive != NULL) {
		return state->cbs.keep_alive(state->user);
	}
	return true;
}

static void ss_set_connection_header(zend_object *response_obj, bool keep_alive)
{
	if (keep_alive) {
		http_response_static_set_header(response_obj, "connection", 10, "keep-alive", 10);
	} else {
		http_response_static_set_header(response_obj, "connection", 10, "close", 5);
	}
}

/* Push the mount's Cache-Control + extra headers onto response_obj.
 * include_content_headers=false on the 304 path (RFC 9110 §15.4.5
 * bars Content-* on Not Modified). */
static void apply_mount_headers(zend_object *response_obj,
								const http_static_handler_t *mount,
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

		/* RFC 9110 §15.4.5: 304 must NOT carry Content-* headers. */
		if (!include_content_headers && ZSTR_LEN(name) >= 8 &&
			strncasecmp(ZSTR_VAL(name), "content-", 8) == 0) {
			continue;
		}

		http_response_static_set_header(response_obj, ZSTR_VAL(name), ZSTR_LEN(name),
										Z_STRVAL_P(value), Z_STRLEN_P(value));
	}
	ZEND_HASH_FOREACH_END();
}

/* Hand the request off to the protocol's send_static_response op.
 *
 * Two file_io flows:
 *   - param non-NULL  : body delivery. The op takes ownership of the
 *                       passed file_io. We've been driving OPEN/STAT
 *                       on this same fd via state->file_io; null the
 *                       slot so finalize doesn't double-dispose.
 *   - param NULL      : head-only / error response. state->file_io
 *                       (if any) holds an open fd we no longer need
 *                       — dispose it before delegating.
 *
 * On rc != 0 the op refused (contract: on_done NOT fired). Restore
 * state->file_io to the param so the caller's finalize disposes
 * whatever the caller still owned. NULL stays NULL. */
static bool ss_delegate_to_protocol(ss_state_t *state, zend_async_io_t *file_io,
									uint64_t body_offset, uint64_t body_length, bool head_only)
{
	zend_object *const response_obj = state->response_obj;
	const http_response_stream_ops_t *const ops = http_response_get_stream_ops(response_obj);
	void *const op_ctx = http_response_get_stream_ctx(response_obj);

	if (UNEXPECTED(ops == NULL || ops->send_static_response == NULL)) {
		/* Protocol doesn't implement static delivery yet (H2/H3 path
		 * pre-plumbing). Caller falls back to the synchronous-populate
		 * path. We DO NOT consume file_io here — caller still owns it. */
		return false;
	}

	/* Detach the OPEN/STAT-phase callback before the op installs its own. */
	if (state->cb != NULL && state->file_io != NULL) {
		(void)state->file_io->event.del_callback(&state->file_io->event, state->cb);
		state->cb = NULL;
	}

	/* Head-only / error path: dispose the OPEN'd fd we held. */
	if (file_io == NULL && state->file_io != NULL) {
		if (state->file_io->event.dispose != NULL) {
			state->file_io->event.dispose(&state->file_io->event);
		}
	}

	state->phase = SS_PHASE_DELEGATED;
	state->file_io = NULL; /* op now owns whatever was passed (incl. NULL) */

	const int rc = ops->send_static_response(op_ctx, response_obj, file_io, body_offset, body_length,
											  head_only, ss_on_protocol_done, state);

	/* WARNING: on rc == 0 with synchronous on_done the op may have
	 * already freed `state` by the time we reach here — read no fields
	 * other than rc. */
	if (UNEXPECTED(rc != 0)) {
		/* Op refused. on_done did not fire → state is still valid.
		 * Restore file_io so finalize disposes what the caller held. */
		state->file_io = file_io;
		state->phase = SS_PHASE_DONE;
		return false;
	}

	return true;
}

/* === Single dispatch callback ===================================== */

static void ss_handle_open(ss_state_t *state, zend_object *exception);
static void ss_handle_stat(ss_state_t *state);
static bool ss_emit_error_via_op(ss_state_t *state, int status, const char *body, size_t body_len);

/* The persistent callback. Registered at kick-off, fires for every
 * NOTIFY on file_io->event during OPEN/STAT. Once we delegate to
 * the protocol op the callback is removed. */
static void ss_dispatch(zend_async_event_t *event, zend_async_event_callback_t *callback,
						void *result, zend_object *exception)
{
	(void)event;
	ss_state_t *const state = ((ss_cb_t *)callback)->state;
	zend_async_io_req_t *const req = (zend_async_io_req_t *)result;

	switch (state->phase) {
	case SS_PHASE_OPEN:
		/* libuv_fs_open notifies with result=NULL — the only valid
		 * signal during this phase. Any non-NULL result is a re-
		 * entrant fire from a later phase's submit, ignore. */
		if (req != NULL) {
			return;
		}
		ss_handle_open(state, exception);
		return;

	case SS_PHASE_STAT:
		if (req == NULL || req != state->pending_req) {
			return;
		}
		state->pending_req = NULL;
		if (req->dispose != NULL) {
			req->dispose(req);
		}
		if (UNEXPECTED(exception != NULL)) {
			/* Map to a 500 emitted through the protocol op (so the
			 * H1 wire write goes through the right channel for plain
			 * vs TLS). */
			if (!ss_emit_error_via_op(state, 500, "Internal Server Error", 21)) {
				ss_finalize(state, -1);
			}
			return;
		}
		ss_handle_stat(state);
		return;

	case SS_PHASE_DELEGATED:
	case SS_PHASE_DONE:
	default:
		return;
	}
}

/* Emit a small text/plain error response through the protocol op.
 * Used for 4xx / 416 / 500 / 413 — bodies are pre-known short text
 * strings. The op's file_io==NULL contract emits inline body off
 * response_obj.
 *
 * Returns true if the protocol op accepted the request — in that
 * case state will be freed via ss_on_protocol_done and the caller
 * MUST NOT call ss_finalize. False means the op refused (no
 * stream_ops or send_static_response==NULL); the caller falls
 * back to ss_finalize, which still runs counters / dispose. */
static bool ss_emit_error_via_op(ss_state_t *state, int status, const char *body, size_t body_len)
{
	zend_object *const response_obj = state->response_obj;
	http_response_static_set_status(response_obj, status);
	http_response_static_set_header(response_obj, "content-type", 12, "text/plain; charset=utf-8",
									25);
	if (body != NULL && body_len > 0) {
		http_response_static_set_body_cstr(response_obj, body, body_len);
	}
	ss_set_content_length(response_obj, (uint64_t)body_len);
	ss_set_connection_header(response_obj, ss_keep_alive(state));

	http_server_count_request(state->counters);
	return ss_delegate_to_protocol(state, NULL, 0, 0, true);
}

/* on_missing:Next rollback (#5c). Open failed on a mount configured to
 * fall through. Tear down the static FSM scratch state and hand off to
 * the protocol-supplied callback so it can spawn its handler
 * coroutine. The caller's on_passthrough_to_php is responsible for
 * dropping whatever resources the matching on_hard_zero_armed pinned —
 * conceptually the static path no longer owns this request, the
 * caller's PHP-handler coroutine does. */
static void ss_rollback_to_php_handler(ss_state_t *state)
{
	/* Drop the file_io + persistent callback (mirrors ss_finalize but
	 * without firing on_static_done — the caller treats the request as
	 * if static had never claimed it). */
	if (state->cb != NULL && state->file_io != NULL) {
		(void)state->file_io->event.del_callback(&state->file_io->event, state->cb);
		state->cb = NULL;
	}

	if (state->file_io != NULL) {
		if (state->file_io->event.dispose != NULL) {
			state->file_io->event.dispose(&state->file_io->event);
		}
		state->file_io = NULL;
	}

	state->phase = SS_PHASE_DONE;

	const http_static_dispatch_cbs_t cbs_copy = state->cbs;
	void *const user = state->user;
	ss_state_free(state);

	if (cbs_copy.on_passthrough_to_php != NULL) {
		cbs_copy.on_passthrough_to_php(user);
	}
}

static void ss_handle_open(ss_state_t *state, zend_object *exception)
{
	if (UNEXPECTED(exception != NULL || (state->file_io->state & ZEND_ASYNC_IO_CLOSED) != 0)) {
		if (state->mount->flags & HTTP_STATIC_FLAG_ON_MISSING_NEXT) {
			ss_rollback_to_php_handler(state);
			return;
		}
		if (!ss_emit_error_via_op(state, 404, "Not Found", 9)) {
			ss_finalize(state, -1);
		}
		return;
	}

	/* Open-file-cache hit: state->st is pre-populated. Skip the
	 * IO_STAT submit entirely — straight into header build with
	 * cached etag/MIME/lm. */
	if (state->has_cached_meta) {
		ss_handle_stat(state);
		return;
	}

	state->phase = SS_PHASE_STAT;
	state->pending_req = ZEND_ASYNC_IO_STAT(state->file_io, &state->st);
	if (UNEXPECTED(state->pending_req == NULL)) {
		if (!ss_emit_error_via_op(state, 500, "Internal Server Error", 21)) {
			ss_finalize(state, -1);
		}
	}
}

static void ss_handle_stat(ss_state_t *state)
{
	zend_object *const response_obj = state->response_obj;

	if (UNEXPECTED(!S_ISREG(state->st.st_mode))) {
		if (!ss_emit_error_via_op(state, 404, "Not Found", 9)) {
			ss_finalize(state, -1);
		}
		return;
	}

	if (UNEXPECTED((uint64_t)state->st.st_size > (uint64_t)HTTP_STATIC_MAX_FILE_SIZE)) {
		if (!ss_emit_error_via_op(state, 413, "Payload Too Large", 17)) {
			ss_finalize(state, -1);
		}
		return;
	}

	/* On a cache hit ss_kick_off pre-rendered etag/lm into the state
	 * scratch buffers. On a miss we synthesise everything from the
	 * freshly-stat'd inode and (at the bottom) hand it to the cache. */
	char etag_buf[HTTP_STATIC_ETAG_BUF_LEN];
	char last_modified_buf[HTTP_STATIC_DATE_BUF_LEN];
	bool etag_enabled;
	const char *content_type = NULL;
	size_t content_type_len = 0;

	if (state->has_cached_meta) {
		etag_enabled = state->cached_etag_enabled;
		if (etag_enabled) {
			memcpy(etag_buf, state->cached_etag_buf, HTTP_STATIC_ETAG_BUF_LEN);
		}
		memcpy(last_modified_buf, state->cached_lm_buf, HTTP_STATIC_DATE_BUF_LEN);
		content_type = state->cached_content_type;
		content_type_len = state->cached_content_type_len;
	} else {
		etag_enabled = (state->mount->flags & HTTP_STATIC_FLAG_ETAG) != 0;
		if (etag_enabled) {
			http_static_etag_format(&state->st, etag_buf);
		}
		http_static_format_http_date(state->st.st_mtime, last_modified_buf);
		if (state->override_content_type != NULL) {
			content_type = state->override_content_type;
			content_type_len = state->override_content_type_len;
		} else if (!http_static_mime_lookup(state->mount, state->fs_path, state->fs_path_len,
											&content_type, &content_type_len)) {
			content_type = "application/octet-stream";
			content_type_len = sizeof("application/octet-stream") - 1;
		}
	}

	const zend_string *if_none_match =
		find_first_request_header(state->request, "if-none-match", 13);
	const zend_string *if_modified_since =
		find_first_request_header(state->request, "if-modified-since", 17);
	const bool not_modified = http_static_conditional_match(
		if_none_match != NULL ? ZSTR_VAL(if_none_match) : NULL,
		if_none_match != NULL ? ZSTR_LEN(if_none_match) : 0,
		if_modified_since != NULL ? ZSTR_VAL(if_modified_since) : NULL,
		if_modified_since != NULL ? ZSTR_LEN(if_modified_since) : 0, etag_enabled ? etag_buf : NULL,
		etag_enabled ? HTTP_STATIC_ETAG_LEN : 0, state->st.st_mtime);

	/* Cache insert (only on miss path). */
	if (!state->has_cached_meta && state->server != NULL) {
		http_static_cache_t *cache = http_static_cache_acquire(state->server);
		if (cache != NULL) {
			http_static_cache_insert(cache, state->fs_path, state->fs_path_len, &state->st,
									 content_type, content_type_len, etag_enabled ? etag_buf : NULL,
									 etag_enabled ? HTTP_STATIC_ETAG_LEN : 0, last_modified_buf,
									 HTTP_STATIC_DATE_LEN);
		}
	}

	/* Range support (RFC 9110 §14.2). */
	state->range_total = (uint64_t)state->st.st_size;
	state->is_range = false;
	if (!not_modified) {
		const zend_string *range_hdr = find_first_request_header(state->request, "range", 5);
		const zend_string *if_range = find_first_request_header(state->request, "if-range", 8);
		bool range_allowed = true;
		if (if_range != NULL && etag_enabled) {
			range_allowed = (ZSTR_LEN(if_range) == HTTP_STATIC_ETAG_LEN &&
							 memcmp(ZSTR_VAL(if_range), etag_buf, HTTP_STATIC_ETAG_LEN) == 0);
		} else if (if_range != NULL) {
			range_allowed = false;
		}
		if (range_hdr != NULL && range_allowed) {
			uint64_t first = 0, last = 0;
			const int rc = parse_byte_range(ZSTR_VAL(range_hdr), ZSTR_LEN(range_hdr),
											state->range_total, &first, &last);
			if (rc == 1) {
				state->is_range = true;
				state->range_first = first;
				state->range_last = last;
			} else if (rc == -1) {
				/* 416. Carry "Content-Range: bytes [star]/size" per RFC 9110 sec 14.1.2. */
				http_response_static_set_status(response_obj, 416);
				http_response_static_set_header(response_obj, "content-type", 12,
												"text/plain; charset=utf-8", 25);
				char cr[48];
				const int crn = snprintf(cr, sizeof(cr), "bytes */%" PRIu64, state->range_total);
				if (crn > 0 && (size_t)crn < sizeof(cr)) {
					http_response_static_set_header(response_obj, "content-range", 13, cr,
													(size_t)crn);
				}
				http_response_static_set_header(response_obj, "content-length", 14, "0", 1);
				ss_set_connection_header(response_obj, ss_keep_alive(state));
				http_server_count_request(state->counters);
				if (!ss_delegate_to_protocol(state, NULL, 0, 0, true)) {
					ss_finalize(state, -1);
				}
				return;
			}
		}
	}

	const bool keep_alive = ss_keep_alive(state);

	/* === Build the response onto response_obj ============================
	 *
	 * The protocol op serializes status + headers verbatim — we
	 * write every header here, including Content-Length / Content-
	 * Range, so the wire output is byte-identical to what the
	 * pre-refactor inline helpers produced. */

	const bool include_content_headers = !not_modified;
	const int status_code = not_modified ? 304 : (state->is_range ? 206 : 200);

	http_response_static_set_status(response_obj, status_code);

	if (include_content_headers && content_type != NULL) {
		http_response_static_set_header(response_obj, "content-type", 12, content_type,
										content_type_len);
	}

	const uint64_t body_len = not_modified
								  ? 0
								  : (state->is_range
										 ? (state->range_last - state->range_first + 1)
										 : (uint64_t)state->st.st_size);

	if (include_content_headers) {
		ss_set_content_length(response_obj, body_len);
	}

	if (etag_enabled) {
		http_response_static_set_header(response_obj, "etag", 4, etag_buf, HTTP_STATIC_ETAG_LEN);
	}
	http_response_static_set_header(response_obj, "last-modified", 13, last_modified_buf,
									HTTP_STATIC_DATE_LEN);

	/* Precompressed sidecar served: tell intermediaries the
	 * representation depends on Accept-Encoding. Vary is added
	 * unconditionally on Content-Encoding-bearing responses; no
	 * harm in extra Vary on the 304 path either. */
	if (state->content_encoding != NULL && include_content_headers) {
		http_response_static_set_header(response_obj, "content-encoding", 16,
										state->content_encoding, state->content_encoding_len);
	}
	if (state->content_encoding != NULL) {
		http_response_static_set_header(response_obj, "vary", 4, "Accept-Encoding", 15);
	}

	/* Range advertise / commit. Accept-Ranges goes on every
	 * 200/206 (and 304); a 206 response also gets Content-Range. */
	if (include_content_headers) {
		http_response_static_set_header(response_obj, "accept-ranges", 13, "bytes", 5);
	}
	if (state->is_range && include_content_headers) {
		char cr[64];
		const int n = snprintf(cr, sizeof(cr), "bytes %" PRIu64 "-%" PRIu64 "/%" PRIu64,
							   state->range_first, state->range_last, state->range_total);
		if (n > 0 && (size_t)n < sizeof(cr)) {
			http_response_static_set_header(response_obj, "content-range", 13, cr, (size_t)n);
		}
	}

	apply_mount_headers(response_obj, state->mount, include_content_headers);
	ss_set_connection_header(response_obj, keep_alive);

	/* === Hand off to the protocol's send_static_response ============== */

	zend_async_io_t *const file_io = state->file_io;

	if (not_modified) {
		http_server_count_request(state->counters);
		if (UNEXPECTED(!ss_delegate_to_protocol(state, file_io, 0, 0, true))) {
			ss_finalize(state, -1);
		}
		return;
	}

	if (state->is_head || state->st.st_size == 0) {
		http_server_count_request(state->counters);
		if (UNEXPECTED(!ss_delegate_to_protocol(state, file_io, 0, 0, true))) {
			ss_finalize(state, -1);
		}
		return;
	}

	const uint64_t body_offset = state->is_range ? state->range_first : 0;
	http_server_count_request(state->counters);
	if (UNEXPECTED(!ss_delegate_to_protocol(state, file_io, body_offset, body_len, false))) {
		ss_finalize(state, -1);
	}
}

/* Protocol op completion. status==0 success; non-zero abort (peer
 * reset / write error). The protocol op has disposed the file_io;
 * finalize fires the caller's on_static_done with the same status. */
static void ss_on_protocol_done(void *user, int status)
{
	ss_state_t *const state = (ss_state_t *)user;
	if (state == NULL) {
		return;
	}
	ss_finalize(state, status);
}

/* === Hard-zero kick-off =========================================== */

/* Take the dispatch hand-off and start the async chain. Returns true
 * on success — caller must return HARD_ZERO. False = setup failed,
 * caller falls back to the synchronous-populate path.
 *
 * `cv` is non-NULL when the pre-flight had an open-file-cache hit:
 * the FSM will skip IO_STAT (state->st pre-filled), etag formatting,
 * MIME lookup and IMF-date formatting on the way to ss_handle_stat. */
static bool ss_kick_off(http_server_object *server, http_request_t *request,
						zend_object *response_obj, http_server_counters_t *counters,
						const http_static_dispatch_cbs_t *cbs, void *user,
						const http_static_handler_t *mount, const char *fs_path, size_t fs_path_len,
						const bool is_head, const http_static_cache_view_t *cv,
						const char *encoding, size_t encoding_len, const char *override_ct,
						size_t override_ct_len)
{
	if (UNEXPECTED(fs_path_len + 1 >= MAXPATHLEN)) {
		return false;
	}

	/* Protocol must implement send_static_response. H2/H3 leave it
	 * NULL today, in which case we fall back to the synchronous-
	 * populate path so the regular dispatch tail handles delivery. */
	const http_response_stream_ops_t *const ops = http_response_get_stream_ops(response_obj);
	if (ops == NULL || ops->send_static_response == NULL) {
		return false;
	}

	ss_state_t *state = ecalloc(1, sizeof(*state));
	state->server = server;
	state->request = request;
	state->response_obj = response_obj;
	state->counters = counters;
	if (cbs != NULL) {
		state->cbs = *cbs;
	}
	state->user = user;
	state->mount = mount;
	state->is_head = is_head;
	state->fs_path = emalloc(fs_path_len + 1);
	memcpy(state->fs_path, fs_path, fs_path_len);
	state->fs_path[fs_path_len] = '\0';
	state->fs_path_len = fs_path_len;

	if (encoding != NULL && encoding_len > 0) {
		state->content_encoding = encoding;
		state->content_encoding_len = encoding_len;
	}
	if (override_ct != NULL && override_ct_len > 0) {
		state->override_content_type = override_ct;
		state->override_content_type_len = override_ct_len;
	}

	if (cv != NULL) {
		state->has_cached_meta = true;
		state->st = cv->st;
		state->cached_content_type = cv->content_type;
		state->cached_content_type_len = cv->content_type_len;
		if (cv->etag != NULL && cv->etag_len == HTTP_STATIC_ETAG_LEN) {
			memcpy(state->cached_etag_buf, cv->etag, HTTP_STATIC_ETAG_LEN);
			state->cached_etag_buf[HTTP_STATIC_ETAG_LEN] = '\0';
			state->cached_etag_enabled = true;
		}
		if (cv->last_modified != NULL && cv->last_modified_len == HTTP_STATIC_DATE_LEN) {
			memcpy(state->cached_lm_buf, cv->last_modified, HTTP_STATIC_DATE_LEN);
			state->cached_lm_buf[HTTP_STATIC_DATE_LEN] = '\0';
		}
	}

	state->phase = SS_PHASE_OPEN;

	state->file_io = ZEND_ASYNC_FS_OPEN(state->fs_path, O_RDONLY | O_CLOEXEC, 0);
	if (UNEXPECTED(state->file_io == NULL)) {
		ss_state_free(state);
		return false;
	}

	/* One persistent callback for the OPEN/STAT phases. The protocol
	 * op replaces it with its own once we delegate. */
	ss_cb_t *cb = (ss_cb_t *)ZEND_ASYNC_EVENT_CALLBACK_EX(ss_dispatch, sizeof(ss_cb_t));
	if (UNEXPECTED(cb == NULL)) {
		if (state->file_io->event.dispose != NULL) {
			state->file_io->event.dispose(&state->file_io->event);
		}
		ss_state_free(state);
		return false;
	}
	cb->base.dispose = ss_cb_dispose;
	cb->state = state;

	if (UNEXPECTED(!state->file_io->event.add_callback(&state->file_io->event, &cb->base))) {
		efree(cb);
		if (state->file_io->event.dispose != NULL) {
			state->file_io->event.dispose(&state->file_io->event);
		}
		ss_state_free(state);
		return false;
	}
	state->cb = &cb->base;

	/* Tell the caller the FSM has armed — it pins protocol-side
	 * resources (refcount conn / bump in-flight counter / set
	 * processing state) here. Paired with on_static_done. */
	if (cbs != NULL && cbs->on_hard_zero_armed != NULL) {
		cbs->on_hard_zero_armed(user);
	}
	state->armed = true;

	/* Telemetry — see http_server_counters_t::static_zero_coroutine_total. */
	http_server_on_static_zero_coroutine(counters);

	return true;
}

/* Single Byte-Range parser. RFC 9110 §14.1.2.
 *
 * Accepts:
 *   bytes=A-B      first..last (inclusive)
 *   bytes=A-       first..size-1
 *   bytes=-N       size-N..size-1 (suffix-length form)
 *
 * Multi-range syntax (comma-separated) is recognised but rejected
 * here — the caller falls back to 200 with the full body, which
 * is RFC-permitted ("a server MAY ignore the Range header field").
 * Multipart/byteranges responses are a separate follow-up.
 *
 * Returns:
 *   1  parsed successfully into *out_first / *out_last (inclusive,
 *      already validated against `size`).
 *   0  header malformed or multi-range — caller serves 200 full body.
 *  -1  syntactically valid but unsatisfiable (start past EOF, etc) —
 *      caller MUST emit 416 Range Not Satisfiable per §14.1.2. */
static int parse_byte_range(const char *hdr, size_t hdr_len, uint64_t size, uint64_t *out_first,
							uint64_t *out_last)
{
	if (hdr == NULL || hdr_len < 7) {
		return 0;
	}
	if (memcmp(hdr, "bytes=", 6) != 0) {
		return 0;
	}
	const char *p = hdr + 6;
	const char *end = hdr + hdr_len;
	while (p < end && (*p == ' ' || *p == '\t')) {
		p++;
	}
	/* Reject multi-range: scan for ',' before the dash. */
	for (const char *q = p; q < end; q++) {
		if (*q == ',') {
			return 0;
		}
	}
	if (p >= end) {
		return 0;
	}

	bool suffix_form = false;
	uint64_t first = 0;
	bool first_set = false;
	if (*p == '-') {
		suffix_form = true;
		p++;
	} else {
		while (p < end && *p >= '0' && *p <= '9') {
			if (first > UINT64_MAX / 10) {
				return 0;
			}
			first = first * 10 + (uint64_t)(*p - '0');
			first_set = true;
			p++;
		}
		if (!first_set || p >= end || *p != '-') {
			return 0;
		}
		p++;
	}

	uint64_t last = 0;
	bool last_set = false;
	while (p < end && *p >= '0' && *p <= '9') {
		if (last > UINT64_MAX / 10) {
			return 0;
		}
		last = last * 10 + (uint64_t)(*p - '0');
		last_set = true;
		p++;
	}
	while (p < end && (*p == ' ' || *p == '\t')) {
		p++;
	}
	if (p != end) {
		return 0; /* trailing garbage */
	}

	if (size == 0) {
		return -1; /* nothing to slice from */
	}

	if (suffix_form) {
		if (!last_set || last == 0) {
			return 0;
		}
		if (last >= size) {
			/* "last N where N >= size" → whole file (RFC 9110: a
			 * suffix-length larger than the resource length is
			 * treated as the whole resource, status still 206). */
			*out_first = 0;
		} else {
			*out_first = size - last;
		}
		*out_last = size - 1;
		return 1;
	}
	if (first >= size) {
		return -1;
	}
	if (!last_set) {
		*out_first = first;
		*out_last = size - 1;
		return 1;
	}
	if (last < first) {
		return 0;
	}
	if (last >= size) {
		last = size - 1;
	}
	*out_first = first;
	*out_last = last;
	return 1;
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
 * *out_encoding. Caller passes both into ss_kick_off so the FSM serves
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

	const zend_string *ae = find_first_request_header(request, "accept-encoding", 15);
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
	const bool is_head = method_is_head(request);
	const bool is_get = method_is_get(request);
	if (!is_get && !is_head) {
		return HTTP_STATIC_PASSTHROUGH;
	}

	/* request->path is built lazily by the PHP-side getter; req->uri is
	 * always populated by the parser. http_static_path_resolve strips
	 * '?' and '#' so the whole URI is safe to feed in. */
	const char *const req_path = (request->uri != NULL) ? ZSTR_VAL(request->uri) : NULL;
	const size_t req_path_len = (request->uri != NULL) ? ZSTR_LEN(request->uri) : 0;
	if (UNEXPECTED(req_path == NULL || req_path_len == 0)) {
		return HTTP_STATIC_PASSTHROUGH;
	}

	for (size_t mi = 0; mi < mount_count; mi++) {
		const http_static_handler_t *const mount = http_static_handler_get(server, mi);
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
			emit_status(response_obj, 400, "Bad Request", 11);
			return HTTP_STATIC_HANDLED;
		}
		/* Dotfile-deny / traversal escape: 404 (not 403) so existence
		 * of the restricted resource isn't disclosed. */
		if (UNEXPECTED(rc == HTTP_STATIC_PATH_FORBIDDEN)) {
			emit_status(response_obj, 404, "Not Found", 9);
			return HTTP_STATIC_HANDLED;
		}
		if (UNEXPECTED(rc == HTTP_STATIC_PATH_HIDE)) {
			if (mount->flags & HTTP_STATIC_FLAG_ON_MISSING_NEXT) {
				return HTTP_STATIC_PASSTHROUGH;
			}
			emit_status(response_obj, 404, "Not Found", 9);
			return HTTP_STATIC_HANDLED;
		}

		/* Hide-globs match against the relative path so operator-
		 * authored patterns target what they see. */
		if (UNEXPECTED(relative_len > 0 &&
					   http_static_path_is_hidden(mount, relative, relative_len))) {
			if (mount->flags & HTTP_STATIC_FLAG_ON_MISSING_NEXT) {
				return HTTP_STATIC_PASSTHROUGH;
			}
			emit_status(response_obj, 404, "Not Found", 9);
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
				const zend_string *const idx = mount->index_files[ii];
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
				emit_status(response_obj, 404, "Not Found", 9);
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
		 * On open-error the on_missing:Next rollback in ss_handle_open
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
		 * and carried as override_ct so ss_handle_stat doesn't redo
		 * the lookup against the .gz extension). */
		const char *picked_encoding = NULL;
		size_t picked_encoding_len = 0;
		const char *override_ct = NULL;
		size_t override_ct_len = 0;
		if ((mount->flags & (HTTP_STATIC_FLAG_PRECOMP_BR | HTTP_STATIC_FLAG_PRECOMP_GZIP |
							 HTTP_STATIC_FLAG_PRECOMP_ZSTD)) != 0) {
			const char *pre_ct = NULL;
			size_t pre_ct_len = 0;
			if (!http_static_mime_lookup(mount, fs_path, fs_path_len, &pre_ct, &pre_ct_len)) {
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
				 * FSM will skip every one of those derivations on the
				 * way through ss_handle_open → ss_handle_stat. */
				http_server_on_static_cache_hit(counters);
				gate_ok = true;
				have_view = true;
			} else {
				http_server_on_static_cache_miss(counters);
				gate_ok =
					symlink_policy_admits(mount, fs_path) && resolved_under_root(mount, fs_path);
				/* Insertion happens in ss_handle_stat once st / etag /
				 * content_type / Last-Modified are all in hand. */
			}
		} else {
			gate_ok = symlink_policy_admits(mount, fs_path) && resolved_under_root(mount, fs_path);
		}
		if (gate_ok) {
			if (ss_kick_off(server, request, response_obj, counters, cbs, user, mount, fs_path,
							fs_path_len, is_head, have_view ? &cv : NULL, picked_encoding,
							picked_encoding_len, override_ct, override_ct_len)) {
				return HTTP_STATIC_HARD_ZERO;
			}
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
			emit_status(response_obj, 404, "Not Found", 9);
			return HTTP_STATIC_HANDLED;
		}

		/* Closes the intermediate-symlink-traversal gap that O_NOFOLLOW
		 * leaves open: realpath() canonicalises every segment, so a
		 * symlink anywhere on the path that points outside the mount
		 * surfaces here as a prefix mismatch. */
		if (UNEXPECTED(!resolved_under_root(mount, fs_path) ||
					   !symlink_policy_admits(mount, fs_path))) {
			close(fd);
			emit_status(response_obj, 404, "Not Found", 9);
			return HTTP_STATIC_HANDLED;
		}

		/* Synchronous slurp cap — protects the loop from a stray giant
		 * file. The async sendfile path will remove this limit. */
		if (UNEXPECTED((uint64_t)st.st_size > (uint64_t)HTTP_STATIC_MAX_FILE_SIZE)) {
			close(fd);
			emit_status(response_obj, 413, "Payload Too Large", 17);
			return HTTP_STATIC_HANDLED;
		}

		char etag_buf[HTTP_STATIC_ETAG_BUF_LEN];
		const bool etag_enabled = (mount->flags & HTTP_STATIC_FLAG_ETAG) != 0;
		if (etag_enabled) {
			http_static_etag_format(&st, etag_buf);
		}

		char last_modified_buf[HTTP_STATIC_DATE_BUF_LEN];
		http_static_format_http_date(st.st_mtime, last_modified_buf);

		const zend_string *const if_none_match = find_first_request_header(request, "if-none-match", 13);
		const zend_string *const if_modified_since =
			find_first_request_header(request, "if-modified-since", 17);
		const bool not_modified = http_static_conditional_match(
			if_none_match != NULL ? ZSTR_VAL(if_none_match) : NULL,
			if_none_match != NULL ? ZSTR_LEN(if_none_match) : 0,
			if_modified_since != NULL ? ZSTR_VAL(if_modified_since) : NULL,
			if_modified_since != NULL ? ZSTR_LEN(if_modified_since) : 0,
			etag_enabled ? etag_buf : NULL, etag_enabled ? HTTP_STATIC_ETAG_LEN : 0, st.st_mtime);

		if (not_modified) {
			close(fd);
			http_response_static_set_status(response_obj, 304);
			if (etag_enabled) {
				http_response_static_set_header(response_obj, "etag", 4, etag_buf,
												HTTP_STATIC_ETAG_LEN);
			}
			http_response_static_set_header(response_obj, "last-modified", 13, last_modified_buf,
											HTTP_STATIC_DATE_LEN);
			apply_mount_headers(response_obj, mount, false);
			return HTTP_STATIC_HANDLED;
		}

		zend_string *body = NULL;
		if (is_get) {
			body = slurp_fd(fd, (size_t)st.st_size);
			if (UNEXPECTED(body == NULL)) {
				close(fd);
				emit_status(response_obj, 500, "Internal Server Error", 21);
				return HTTP_STATIC_HANDLED;
			}
		}
		close(fd);

		http_response_static_set_status(response_obj, 200);

		const char *content_type = NULL;
		size_t content_type_len = 0;
		if (!http_static_mime_lookup(mount, fs_path, fs_path_len, &content_type,
									 &content_type_len)) {
			content_type = "application/octet-stream";
			content_type_len = sizeof("application/octet-stream") - 1;
		}
		http_response_static_set_header(response_obj, "content-type", 12, content_type,
										content_type_len);

		if (etag_enabled) {
			http_response_static_set_header(response_obj, "etag", 4, etag_buf,
											HTTP_STATIC_ETAG_LEN);
		}
		http_response_static_set_header(response_obj, "last-modified", 13, last_modified_buf,
										HTTP_STATIC_DATE_LEN);
		apply_mount_headers(response_obj, mount, true);

		if (is_head) {
			/* The format-time path computes Content-Length from the
			 * body smart_str; with an empty body we have to advertise
			 * the would-be size explicitly. */
			ss_set_content_length(response_obj, (uint64_t)st.st_size);
		} else {
			http_response_static_set_body_str(response_obj, body);
			zend_string_release(body);
		}

		return HTTP_STATIC_HANDLED;
	}

	return HTTP_STATIC_PASSTHROUGH;
}
