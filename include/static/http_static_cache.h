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
#include <sys/stat.h>
#include <time.h>

typedef struct http_static_cache_s http_static_cache_t;

/* Allocate a cache. max_entries == 0 makes lookup/insert no-ops
 * (caching disabled). ttl_seconds <= 0 also disables. Returned
 * pointer is freed by http_static_cache_destroy. */
http_static_cache_t *http_static_cache_create(size_t max_entries,
                                              time_t ttl_seconds);
void                 http_static_cache_destroy(http_static_cache_t *cache);

/* Cache entry view returned to the caller on hit. All const-pointed
 * data is owned by the cache and lives until eviction; caller must
 * NOT free or retain past the current request. */
typedef struct {
    struct stat  st;
    const char  *content_type;        /* persistent — points into MIME table */
    size_t       content_type_len;
    const char  *etag;                /* NULL if etag disabled at insert time */
    size_t       etag_len;            /* 0 if no etag */
    const char  *last_modified;       /* persistent for entry lifetime */
    size_t       last_modified_len;
} http_static_cache_view_t;

/* Lookup. Returns true on hit + populates *out_view. False on miss
 * or expired entry (entry is removed in that case). path is the
 * absolute resolved on-disk path (post-realpath). */
bool http_static_cache_lookup(http_static_cache_t *cache,
                              const char *path, size_t path_len,
                              http_static_cache_view_t *out_view);

/* Insert / replace. Caller passes pre-computed metadata; the cache
 * copies/owns everything. mime/etag/last_modified buffers are
 * memcpy'd into the entry. content_type pointer is borrowed (it
 * lives in the persistent MIME table or is the literal
 * "application/octet-stream" — both safe to alias). Eviction picks
 * the LRU tail when at capacity. */
void http_static_cache_insert(http_static_cache_t *cache,
                              const char *path, size_t path_len,
                              const struct stat *st,
                              const char *content_type, size_t content_type_len,
                              const char *etag,          size_t etag_len,
                              const char *last_modified, size_t last_modified_len);

/* Forcibly drop all entries — used at shutdown / config reload. */
void http_static_cache_clear(http_static_cache_t *cache);

#endif /* HTTP_STATIC_CACHE_H */
