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

/* Protocol-side dispatch hooks. Aliased to send_file_cbs_t — the static
 * dispatcher hands off to the same engine HttpResponse::sendFile() uses,
 * so the cbs shape is identical. Field meanings (callback semantics):
 *
 * on_armed       — fires once the engine kicks off its async chain (the
 *                  synchronous tail of try_serve that returns HARD_ZERO).
 *                  The caller pins protocol-side resources here.
 * on_done        — fires when the protocol's send_static_response op
 *                  finishes (or engine failed before delegating).
 *                  status==0 ok, non-zero abort. Drop pinned resources.
 * on_passthrough — on_missing:Next rollback. Caller spawns its PHP
 *                  handler coroutine. NULL is invalid for mounts that
 *                  opt into HTTP_STATIC_FLAG_ON_MISSING_NEXT.
 * keep_alive     — per-protocol keep-alive verdict. NULL → assume true. */
#include "send_file.h"
typedef send_file_cbs_t http_static_dispatch_cbs_t;

/* Dispatch hook entrypoint. Protocol-agnostic — caller supplies the
 * response object, the counters block (for telemetry, may be NULL on
 * test paths), the callback set, and a verbatim user pointer. */
http_static_result_t http_static_try_serve(http_server_object *server,
										   struct http_request_t *request,
										   zend_object *response_obj,
										   http_server_counters_t *counters,
										   const http_static_dispatch_cbs_t *cbs,
										   void *user);

/* Server-free core of the dispatch hook. Identical logic to
 * http_static_try_serve but keyed on a borrowed mount array + an
 * explicit open-file cache (NULL = uncached) instead of the PHP server
 * object — the transport reactor has no server object on its thread but
 * can hold its own refs to the persistent, atomically-refcounted mounts.
 * http_static_try_serve is a thin wrapper that resolves these from the
 * server and forwards. */
struct http_static_cache_s;
http_static_result_t http_static_try_serve_mounts(
	const http_static_handler_t *const *mounts, size_t mount_count,
	struct http_static_cache_s *cache,
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

/* Borrow the server's contiguous mount-pointer array (length ==
 * http_static_handler_count). NULL when there are no mounts. Stable for
 * the server's lifetime. Lets a caller pass the whole array to
 * http_static_try_serve_mounts without the server struct layout. */
const http_static_handler_t *const *
http_static_handler_mounts(const http_server_object *server);

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
