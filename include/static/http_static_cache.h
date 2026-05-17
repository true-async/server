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
/* Persistent body cached on the metadata entry. Returns addref'd
 * zend_string on hit, NULL otherwise. */
zend_string *http_static_cache_body_acquire(http_static_cache_t *cache, const char *path,
											size_t path_len);

/* Persistent-copies body bytes into the entry. No-op if entry missing
 * or already has a body. */
void http_static_cache_body_store(http_static_cache_t *cache, const char *path, size_t path_len,
								  const char *body, size_t body_len);

/* === Existence probe (nginx open_file_cache style) =============
 *
 * The precompressed-sidecar selector needs to ask "does foo.js.gz
 * exist?" before every request. Doing it with stat() is the dominant
 * cost on warm precomp HITs (~20 µs per request on the static-h2
 * benchmark). The probe API reuses the same HashTable with two extra
 * states per entry — positive (the normal cached metadata), and
 * negative ("we checked, file is absent") — so subsequent probes for
 * the same path skip the syscall. Negative entries respect the same
 * TTL as positives. */
typedef enum
{
	HTTP_STATIC_CACHE_PROBE_UNKNOWN = 0, /* no entry / TTL expired */
	HTTP_STATIC_CACHE_PROBE_EXISTS,	     /* positive cache hit */
	HTTP_STATIC_CACHE_PROBE_NOT_FOUND,   /* negative cache hit */
} http_static_cache_probe_t;

/* Pure read — does not mutate LRU/TTL state. Use this in the selector's
 * hot loop where the caller hasn't yet decided whether to act on the
 * answer. */
http_static_cache_probe_t http_static_cache_probe(http_static_cache_t *cache, const char *path,
												  size_t path_len);

/* Record a negative result (the stat we just ran said "no"). Memory
 * footprint is one entry — no body, no etag, no MIME — so negatives
 * cost ~80 bytes apiece. Eviction is the shared LRU at max_entries. */
void http_static_cache_negative_insert(http_static_cache_t *cache, const char *path,
									   size_t path_len);

#endif /* HTTP_STATIC_CACHE_H */
