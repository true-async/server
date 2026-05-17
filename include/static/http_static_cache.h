/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

#ifndef HTTP_STATIC_CACHE_H
#define HTTP_STATIC_CACHE_H

/* nginx-style open_file_cache: bounded LRU of recent path lookups.
 * Stores the realpath result, fstat metadata, MIME content_type, ETag
 * and Last-Modified buffers — every byte the static FSM derives once
 * synchronously per request from the on-disk path. On a cache hit, all
 * of those are reused: realpath is skipped (the dominant cost — N
 * stat() syscalls per path component on a cold dentry cache), the
 * MIME table binary search is skipped, and ETag / Last-Modified
 * snprintf is skipped.
 *
 * Cache is keyed by absolute resolved path. Per-server: every worker
 * has its own cache, no cross-worker sharing. TTL is the only
 * invalidation: after `ttl_seconds` an entry is treated as miss and
 * the path re-validated. Mtime-on-hit re-stat is intentionally NOT
 * done — the whole point is to skip the syscall — operators set TTL
 * based on how stale they tolerate being. Default TTL 60s matches
 * nginx open_file_cache_valid 60s.
 *
 * Concurrent access: NONE. Single-thread per event loop is the
 * invariant. Worker pool gives each worker its own cache, no locking.
 */

#include "php.h"
#include "Zend/zend_stream.h"  /* zend_stat_t */
#include "Zend/zend_async_API.h" /* zend_async_event_t */
#include <time.h>

typedef struct http_static_cache_s http_static_cache_t;

/* Allocate a cache. max_entries == 0 makes lookup/insert no-ops
 * (caching disabled). ttl_seconds <= 0 also disables. Returned
 * pointer is freed by http_static_cache_destroy. */
http_static_cache_t *http_static_cache_create(size_t max_entries, time_t ttl_seconds);
void http_static_cache_destroy(http_static_cache_t *cache);

/* Cache entry view returned to the caller on hit. All const-pointed
 * data is owned by the cache and lives until eviction; caller must
 * NOT free or retain past the current request. */
typedef struct
{
	zend_stat_t st;
	const char *content_type; /* persistent — points into MIME table */
	size_t content_type_len;
	const char *etag;		   /* NULL if etag disabled at insert time */
	size_t etag_len;		   /* 0 if no etag */
	const char *last_modified; /* persistent for entry lifetime */
	size_t last_modified_len;
} http_static_cache_view_t;

/* Lookup. Returns true on hit + populates *out_view. False on miss
 * or expired entry (entry is removed in that case). path is the
 * absolute resolved on-disk path (post-realpath). */
bool http_static_cache_lookup(http_static_cache_t *cache, const char *path, size_t path_len,
							  http_static_cache_view_t *out_view);

/* Insert / replace. Caller passes pre-computed metadata; the cache
 * copies/owns everything. mime/etag/last_modified buffers are
 * memcpy'd into the entry. content_type pointer is borrowed (it
 * lives in the persistent MIME table or is the literal
 * "application/octet-stream" — both safe to alias). Eviction picks
 * the LRU tail when at capacity. */
void http_static_cache_insert(http_static_cache_t *cache, const char *path, size_t path_len,
							  const zend_stat_t *st, const char *content_type,
							  size_t content_type_len, const char *etag, size_t etag_len,
							  const char *last_modified, size_t last_modified_len);

/* Forcibly drop all entries — used at shutdown / config reload. */
void http_static_cache_clear(http_static_cache_t *cache);

/* === Body dedup ================================================== */
/* Single-flight in-memory body cache for the inline-slurp path: when
 * N streams ask for the same small file, only the first reads — the
 * rest reuse the persistent zend_string buffer. Refcount-only, no
 * separate budget: entry holds one ref, each emitting stream addrefs
 * its own; eviction releases the entry's ref, in-flight streams keep
 * the buffer alive until the last release. */

typedef enum
{
	HTTP_STATIC_BODY_HIT,	   /* *out_body addref'd; caller owns the ref */
	HTTP_STATIC_BODY_MISS,	   /* metadata entry exists but no body; caller should begin_load */
	HTTP_STATIC_BODY_INFLIGHT, /* loader in progress; *out_event set, caller awaits */
	HTTP_STATIC_BODY_FAILED,   /* last load failed (errno in *out_err); caller should retry */
	HTTP_STATIC_BODY_NO_ENTRY  /* no metadata entry — body cache N/A for this path */
} http_static_body_status_t;

/* Acquire body for a previously cached path. Outputs depend on status:
 *   HIT      → *out_body = addref'd zend_string (caller releases).
 *   INFLIGHT → *out_event = the loader's ready event (caller awaits).
 *   FAILED   → *out_err = errno; cache clears the failure marker.
 *   MISS/NO_ENTRY → all outputs untouched.
 * Safe to call with NULL out_body/out_event/out_err. */
http_static_body_status_t http_static_cache_body_acquire(
	http_static_cache_t *cache, const char *path, size_t path_len,
	zend_string **out_body, zend_async_event_t **out_event, int *out_err);

/* Mark in-flight before slurping. Creates the ready event if missing.
 * Must be called only after a MISS from body_acquire (i.e. metadata
 * entry exists and body is NULL). Returns true if this caller is the
 * loader (must call publish or fail); false if cache lost the entry
 * or another loader raced in (caller should fall back to private
 * slurp without publishing). */
bool http_static_cache_body_begin_load(http_static_cache_t *cache, const char *path, size_t path_len);

/* Publish a successfully slurped body. Cache makes a persistent copy
 * (one extra memcpy) and stores it; fires + disposes the ready event
 * so parked waiters wake and see HIT. If the metadata entry was
 * evicted in the meantime, publish is a no-op (loader keeps its own
 * copy for the current response). */
void http_static_cache_body_publish(http_static_cache_t *cache, const char *path, size_t path_len,
									const char *body, size_t body_len);

/* Publish a load failure (errno). Waiters wake and see FAILED →
 * fall back to private slurp. */
void http_static_cache_body_fail(http_static_cache_t *cache, const char *path, size_t path_len,
								 int err);

#endif /* HTTP_STATIC_CACHE_H */
