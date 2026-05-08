/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

/* StaticHandler — built-in static file handler (issue #13).
 *
 * One StaticHandler describes one URL-prefix-rooted static mount.
 * Attached to an HttpServer via HttpServer::addStaticHandler(), at
 * which point it transitions to "locked" — all setters reject
 * further mutation. The dispatch hook in http_connection.c walks
 * the locked array during request dispatch and decides whether to
 * serve in C or fall through to the PHP handler. */

#ifndef TRUE_ASYNC_STATIC_HANDLER_H
#define TRUE_ASYNC_STATIC_HANDLER_H

#include "php.h"
#include "zend_smart_str.h"
#include "php_http_server.h" /* http_server_counters_t */
#include <stdbool.h>
#include <stdint.h>

/* Behaviour bits packed into http_static_handler_t::flags. One cache-line
 * load on the dispatch fast path; no per-bool loads. */
#define HTTP_STATIC_FLAG_DOTFILES_DENY (1u << 0) /* default */
#define HTTP_STATIC_FLAG_DOTFILES_ALLOW (1u << 1)
#define HTTP_STATIC_FLAG_DOTFILES_IGNORE (1u << 2)
#define HTTP_STATIC_FLAG_SYMLINKS_REJECT (1u << 3) /* default */
#define HTTP_STATIC_FLAG_SYMLINKS_FOLLOW (1u << 4)
#define HTTP_STATIC_FLAG_SYMLINKS_OWNER (1u << 5)
#define HTTP_STATIC_FLAG_PRECOMP_BR (1u << 6)
#define HTTP_STATIC_FLAG_PRECOMP_GZIP (1u << 7)
#define HTTP_STATIC_FLAG_PRECOMP_ZSTD (1u << 8)
#define HTTP_STATIC_FLAG_ETAG (1u << 9) /* default on */
#define HTTP_STATIC_FLAG_BROWSE (1u << 10)
#define HTTP_STATIC_FLAG_ON_MISSING_NEXT (1u << 11)
#define HTTP_STATIC_FLAG_LOCKED (1u << 12)

/* Persistent mount descriptor. One copy lives inside each StaticHandler
 * PHP object; addStaticHandler() locks the object and the server stores
 * a pointer to the same struct. The array on the server is therefore
 * read-only after start().
 *
 * Strings are zend_strings (refcounted, may be persistent or not — every
 * setter takes its own ref). hide_globs / index_files are heap arrays of
 * refcounted zend_strings. extra_headers / mime_overrides are HashTables
 * (pointer-stable across pre-lock mutations). */
typedef struct
{
	/* URL prefix (always starts and ends with '/') and its length, kept
	 * separately so the dispatch fast path can do a single memcmp. */
	zend_string *url_prefix;
	size_t url_prefix_len;

	/* Filesystem root, canonicalised at attach time. */
	zend_string *root_directory;

	/* Optional pre-formatted Cache-Control header value. NULL = no header. */
	zend_string *cache_control;

	/* Index file candidates. NULL pointer + index_count == 0 means
	 * "directory request → 404 / passthrough". */
	zend_string **index_files;
	size_t index_count;

	/* Hide-glob array. Matched against the path RELATIVE to root. */
	zend_string **hide_globs;
	size_t hide_count;

	/* Extra response headers (set via setHeader). HashTable<lower-name,
	 * IS_STRING value>. NULL until the first setHeader call. */
	HashTable *extra_headers;

	/* Per-mount MIME overrides: HashTable<lower-extension, IS_STRING
	 * content-type>. NULL until the first setMimeType call. Looked up
	 * BEFORE the built-in table so an operator can override a default
	 * mapping (e.g. force `application/wasm` on a host whose built-in
	 * table is older). */
	HashTable *mime_overrides;

	/* Pre-rendered extra-header block (Cache-Control + every entry of
	 * extra_headers, joined as "Name: value\r\n"). Populated only on
	 * frozen snapshots — NULL on the user-side draft. The hot path on
	 * the hard-zero serve splices these in with one smart_str_append
	 * instead of re-iterating extra_headers per request:
	 *
	 *   prebaked_headers_full        — used on 200/206/etc.
	 *   prebaked_headers_no_content  — used on 304 (RFC 9110 §15.4.5
	 *                                  bars Content-* on Not Modified).
	 *
	 * NULL when neither extra_headers nor cache_control is set. */
	zend_string *prebaked_headers_full;
	zend_string *prebaked_headers_no_content;

	uint32_t flags;

	/* Open-file cache configuration (issue #13 §5a, nginx-style
	 * open_file_cache). Per-mount setters write here; the server-side
	 * cache instance (one per worker) takes the effective settings on
	 * first request — see http_static_cache_acquire() for the merge
	 * across multiple mounts.
	 *
	 * cache_max_entries == 0 disables for this mount.
	 * cache_ttl_seconds  == 0 disables for this mount (an entry would
	 *                         be evicted on every lookup, pointless).
	 *
	 * Defaults at construction time: both 0 (cache off). Operators
	 * opt in via StaticHandler::setOpenFileCache($max, $ttl). */
	int32_t cache_max_entries;
	int32_t cache_ttl_seconds;
} http_static_handler_t;

/* Result of the dispatch attempt.
 *
 * PASSTHROUGH — no static mount matched (or `on_missing: Next` fell
 *               through). Dispatcher continues with the regular
 *               coroutine + PHP handler path.
 *
 * HANDLED     — response_obj populated synchronously (4xx error body,
 *               or a soft-skip mode). Dispatcher proceeds with the
 *               normal coroutine path / dispose flush; the caller is
 *               responsible for skipping its user handler (e.g. set
 *               ctx->skip_php_handler on H1).
 *
 * HARD_ZERO   — owns the request lifecycle from here on. Dispatcher
 *               MUST NOT spawn a coroutine: a callback chain is
 *               already in flight that will emit the response and
 *               run http_request_finalize on its own. Used by the
 *               sendfile path (fs_open → fstat → write headers →
 *               sendfile → close → finalize).
 *
 * ERROR       — reserved.
 */
typedef enum
{
	HTTP_STATIC_PASSTHROUGH = 0,
	HTTP_STATIC_HANDLED = 1,
	HTTP_STATIC_ERROR = 2,
	HTTP_STATIC_HARD_ZERO = 3,
} http_static_result_t;

/* Forward decls — concrete types live in C TUs that include this.
 * Names match those in src/core/http_connection.h and the public
 * php_http_server.h. */
struct http_request_t;
struct _http_connection_t;

/* PHP class entries — filled in by the static_handler_class_register()
 * call from MINIT. */
extern zend_class_entry *http_static_handler_ce;
extern zend_class_entry *http_static_on_missing_ce;
extern zend_class_entry *http_static_dotfiles_ce;
extern zend_class_entry *http_static_symlinks_ce;

/* MINIT entrypoint. */
void http_static_handler_class_register(void);

/* Pull the underlying mount descriptor out of a PHP StaticHandler
 * object. NULL if the zend_object isn't a StaticHandler. */
http_static_handler_t *http_static_handler_from_obj(zend_object *obj);

/* Lock the handler (called from HttpServer::addStaticHandler before the
 * descriptor pointer is stored on the server). After locking, every
 * setter throws HttpServerRuntimeException. */
void http_static_handler_lock(http_static_handler_t *handler);

/* Release every refcounted/owned field on the descriptor in-place. Used by
 * the StaticHandler PHP object's free_obj. Frees emalloc-side fields. */
void http_static_handler_descriptor_destroy(http_static_handler_t *handler);

/* === Persistent shared snapshot ========================================
 *
 * On addStaticHandler the user-side draft mount is "frozen" into a
 * persistent (pemalloc) refcounted snapshot. The server stores a
 * pointer to the embedded http_static_handler_t inside the snapshot;
 * worker-pool TRANSFER then becomes a pointer copy + addref. Cross-
 * thread sharing is safe because every owned field (zend_strings,
 * HashTables) is allocated persistent (refcount frozen at 1) and the
 * snapshot is read-only after the locked-flag is set on freeze. */

/* Allocate and populate a persistent refcounted snapshot from a
 * (locked) draft descriptor. The returned pointer has refcount=1 and
 * points to the http_static_handler_t embedded inside the wrapper —
 * cast-compatible with all reader code on the dispatch hot path.
 * Returns NULL on allocation failure. */
http_static_handler_t *http_static_handler_freeze(const http_static_handler_t *draft);

/* Atomic-addref / release on the shared snapshot. The mount pointer
 * MUST have been produced by http_static_handler_freeze. release() on
 * the last ref destroys all owned fields and pefrees the snapshot. */
void http_static_handler_shared_addref(http_static_handler_t *mount);
void http_static_handler_shared_release(http_static_handler_t *mount);

/* Protocol-agnostic dispatch callbacks. The static handler resolves
 * the mount, opens + stats the file, populates response_obj headers/
 * status/inline body, then either:
 *   - HARD_ZERO: arms the protocol's send_static_response op for body
 *     delivery; on_hard_zero_armed fires before the op is kicked,
 *     on_static_done fires once delivery completes (success or error).
 *   - HANDLED: response_obj is populated and the caller's normal flush
 *     path emits it.
 *   - PASSTHROUGH: nothing claimed; caller runs its PHP handler.
 *   - ENOENT on `on_missing: Next` mount: the static FSM tears down its
 *     scratch state and fires on_passthrough_to_php so the caller
 *     spawns its handler coroutine.
 *
 * Callbacks are invoked from various points inside try_serve and from
 * the FSM continuations rooted at ZEND_ASYNC_FS_OPEN. The user pointer
 * is passed back to every callback verbatim. */
typedef struct {
	/* Called once the static FSM has kicked off its async chain (the
	 * synchronous tail of try_serve that returns HARD_ZERO). The caller
	 * pins protocol-side resources (refcount the conn, bump in-flight
	 * dispatch counter, etc.) — these stay pinned until on_static_done
	 * fires. NULL = nothing to do. */
	void (*on_hard_zero_armed)(void *user);

	/* Called once the protocol's send_static_response op has finished
	 * (or the static handler failed before delegating). status==0 ok,
	 * non-zero abort. The caller drops the resources it pinned in
	 * on_hard_zero_armed and runs whatever post-request bookkeeping it
	 * needs (finalize / keep-alive). NULL = nothing to do, but in that
	 * case file_io leaks unless the protocol op took ownership.
	 *
	 * After this call returns, `user` should be considered freed by
	 * the caller — the static handler will not touch it again. */
	void (*on_static_done)(void *user, int status);

	/* on_missing:Next rollback. The mount said ENOENT-falls-through;
	 * the FSM has torn down its scratch state and is asking the caller
	 * to spawn its PHP-handler coroutine as if try_serve had returned
	 * PASSTHROUGH from the start. NULL is invalid for mounts that opt
	 * into HTTP_STATIC_FLAG_ON_MISSING_NEXT (the static handler has no
	 * other way to recover); for callers that don't support fallthrough
	 * the safest stub closes the request as 404. */
	void (*on_passthrough_to_php)(void *user);

	/* Per-protocol keep-alive verdict — H1 reads conn->keep_alive, H2/
	 * H3 always returns true (multiplexed transport). The static handler
	 * uses this to decide whether to set Connection: keep-alive vs
	 * close. NULL → assume true. */
	bool (*keep_alive)(void *user);
} http_static_dispatch_cbs_t;

/* Dispatch hook entrypoint. Protocol-agnostic — caller supplies the
 * response object, the counters block (for telemetry, may be NULL on
 * test paths), the callback set, and a verbatim user pointer. */
http_static_result_t http_static_try_serve(http_server_object *server,
										   struct http_request_t *request,
										   zend_object *response_obj,
										   http_server_counters_t *counters,
										   const http_static_dispatch_cbs_t *cbs,
										   void *user);

/* Out-of-line "is any mount registered" helper. The struct layout
 * lives in http_server_class.c so the count is not directly visible to
 * the dispatcher TU; this getter is the single authority and is cheap
 * (one load on the hot path). Returns 0 when server is NULL. */
size_t http_static_handler_count(const http_server_object *server);

/* Borrow the read-only mount descriptor at `index` (0-based, must be
 * < http_static_handler_count). Pointer is stable for the lifetime of
 * the server. Used by the dispatch FSM to iterate without seeing the
 * server-object struct layout. */
const http_static_handler_t *http_static_handler_get(const http_server_object *server,
													 size_t index);

/* Open file cache accessor — lazily creates the cache on first call,
 * returns it on subsequent calls. NULL if the server is NULL or
 * allocation failed. Lifetime: until http_server_free destroys the
 * server. The cache is private per server-object — workers each get
 * their own copy after worker-pool transfer. See
 * include/static/http_static_cache.h for cache semantics. */
struct http_static_cache_s;
struct http_static_cache_s *http_static_cache_acquire(http_server_object *server);

/* Maximum file size served on the synchronous read path. Larger files
 * passthrough to the regular handler (or in a future iteration, to a
 * streaming async path). 256 MiB is a comfortable upper bound for
 * typical static asset deployments and protects against a stray giant
 * file blocking the loop on a synchronous read. */
#define HTTP_STATIC_MAX_FILE_SIZE ((size_t)256u * 1024u * 1024u)

#endif /* TRUE_ASYNC_STATIC_HANDLER_H */
