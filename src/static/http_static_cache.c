/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+

  Open file cache (issue #13 follow-up). HashTable keyed by absolute
  path → entry; entries also live in a doubly-linked LRU list with
  head=most-recent, tail=eviction-target. On lookup hit we promote
  the entry to head; on miss / expired we drop and the caller falls
  through to the sync realpath/stat/mime path.

  Single-thread per event loop — no locking.
*/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "php.h"
#include "zend_smart_str.h"

#include "static/http_static_cache.h"

#include <string.h>
#include <time.h>

typedef struct entry_s
{
	/* HashTable holds the keystring; key bytes equal the entry's
	 * resolved-path. The key zend_string is owned by the index
	 * HashTable (it released on entry removal). */
	zend_string *path_key;

	zend_stat_t st;
	time_t cached_at;

	/* content_type — deep-copied into pemalloc'd buffer. Earlier
	 * versions borrowed the pointer assuming it always lived in the
	 * persistent MIME table, but the precompressed-sidecar path
	 * synthesizes a transient zend_string for the override Content-Type
	 * (so the .br/.gz response advertises the ORIGINAL file's MIME).
	 * That pointer is released the moment send_file unwinds, leaving
	 * the cache holding dangling memory and triggering UAF on the
	 * next cache hit (h2 static-h2 precompressed >64K death-spiral). */
	char *content_type;
	size_t content_type_len;

	/* ETag and Last-Modified are pre-formatted ASCII; deep-copied
	 * into pemalloc'd buffers so the entry survives independently of
	 * the original request stack scratch. NULL etag pointer means
	 * caller had etag disabled at insert time. */
	char *etag;
	size_t etag_len;
	char *last_modified;
	size_t last_modified_len;

	/* Persistent body for inline-slurp dedup; NULL until first reader stores it. */
	zend_string *body;

	/* nginx open_file_cache 'errors on' analogue: when set, this entry
	 * is a negative-cache marker — we stat()'d the path and the file
	 * was absent. cache_lookup ignores these (returns miss), so the
	 * engine never tries to serve from them; only the precomp existence
	 * probe consults them. */
	bool not_found;

	struct entry_s *prev; /* LRU links: NULL on head */
	struct entry_s *next; /* NULL on tail */
} entry_t;

struct http_static_cache_s
{
	HashTable index; /* zend_string * → entry_t * */
	entry_t *head;	 /* MRU */
	entry_t *tail;	 /* LRU eviction target */
	size_t count;
	size_t max_entries;
	time_t ttl_seconds;
};

/* === Internal helpers ============================================ */

static void entry_free(entry_t *e)
{
	if (e == NULL) {
		return;
	}

	if (e->path_key != NULL) {
		zend_string_release(e->path_key);
		e->path_key = NULL;
	}

	if (e->content_type != NULL) {
		pefree(e->content_type, 1);
		e->content_type = NULL;
	}

	if (e->etag != NULL) {
		pefree(e->etag, 1);
		e->etag = NULL;
	}

	if (e->last_modified != NULL) {
		pefree(e->last_modified, 1);
		e->last_modified = NULL;
	}

	if (e->body != NULL) {
		zend_string_release(e->body);
		e->body = NULL;
	}

	pefree(e, 1);
}

/* HashTable destructor — fires on remove / clear / destroy. */
static void index_dtor(zval *pZv)
{
	entry_t *e = (entry_t *)Z_PTR_P(pZv);
	entry_free(e);
}


static void lru_unlink(http_static_cache_t *cache, entry_t *e)
{
	if (e->prev != NULL) {
		e->prev->next = e->next;
	} else {
		cache->head = e->next;
	}

	if (e->next != NULL) {
		e->next->prev = e->prev;
	} else {
		cache->tail = e->prev;
	}

	e->prev = e->next = NULL;
}

static void lru_push_head(http_static_cache_t *cache, entry_t *e)
{
	e->prev = NULL;
	e->next = cache->head;

	if (cache->head != NULL) {
		cache->head->prev = e;
	} else {
		/* empty list: new entry is also the tail. */
		cache->tail = e;
	}

	cache->head = e;
}

static void lru_promote(http_static_cache_t *cache, entry_t *e)
{
	if (cache->head == e) {
		return;
	}

	lru_unlink(cache, e);
	lru_push_head(cache, e);
}

/* Drop entry from cache. Removes from HashTable (which fires
 * index_dtor → entry_free) and unlinks from LRU. */
static void evict_entry(http_static_cache_t *cache, entry_t *e)
{
	lru_unlink(cache, e);
	cache->count--;
	/* HashTable removal triggers index_dtor → entry_free; e is dead
	 * after this call. */
	zend_hash_del(&cache->index, e->path_key);
}

static void evict_lru(http_static_cache_t *cache)
{
	if (cache->tail != NULL) {
		evict_entry(cache, cache->tail);
	}
}

/* === Public API ================================================== */

http_static_cache_t *http_static_cache_create(size_t max_entries, time_t ttl_seconds)
{
	http_static_cache_t *cache = pecalloc(1, sizeof(*cache), 1);
	cache->max_entries = max_entries;
	cache->ttl_seconds = ttl_seconds;
	/* Persistent HashTable (lives across requests / workers). */
	zend_hash_init(&cache->index, (uint32_t)(max_entries > 0 ? max_entries : 16), NULL, index_dtor, 1);
	return cache;
}

void http_static_cache_destroy(http_static_cache_t *cache)
{
	if (cache == NULL) {
		return;
	}
	/* HashTable destruction fires index_dtor on every entry. The
	 * LRU links go away naturally because every entry is freed. */
	zend_hash_destroy(&cache->index);
	cache->head = cache->tail = NULL;
	cache->count = 0;
	pefree(cache, 1);
}

void http_static_cache_clear(http_static_cache_t *cache)
{
	ZEND_ASSERT(cache != NULL);
	zend_hash_clean(&cache->index);
	cache->head = cache->tail = NULL;
	cache->count = 0;
}

/* Cache disabled by config, or an empty path — both mean "skip the cache".
 * A runtime feature flag, not a contract violation: callers early-return. */
static bool cache_disabled(const http_static_cache_t *cache, const size_t path_len)
{
	return cache->max_entries == 0 || cache->ttl_seconds <= 0 || path_len == 0;
}

bool http_static_cache_lookup(http_static_cache_t *cache, const char *path, size_t path_len,
							  http_static_cache_view_t *out_view)
{
	ZEND_ASSERT(cache != NULL);
	ZEND_ASSERT(path != NULL);
	ZEND_ASSERT(out_view != NULL);
	/* Cache disabled by config — runtime feature flag, not a contract
	 * violation. Keep as a normal early-return. */
	if (cache_disabled(cache, path_len)) {
		return false;
	}

	/* Hot path: hash directly off the caller-supplied bytes. Avoids
	 * allocating a transient zend_string per lookup that we'd just
	 * release a syscall later. */
	entry_t *e = (entry_t *)zend_hash_str_find_ptr(&cache->index, path, path_len);

	if (e == NULL) {
		return false;
	}

	/* Negative entries are visible only to the existence probe; the
	 * engine has no metadata to serve from them, so treat as miss. */
	if (e->not_found) {
		return false;
	}

	/* TTL check. time(NULL) is a vDSO call on Linux — sub-100 ns,
	 * still cheap relative to realpath we're avoiding. */
	const time_t now = time(NULL);

	if (now - e->cached_at > cache->ttl_seconds) {
		evict_entry(cache, e);
		return false;
	}

	/* Promote on hit. */
	lru_promote(cache, e);

	/* Populate caller's view. All pointers reference live cache
	 * memory — caller must consume them within the request. */
	out_view->st = e->st;
	out_view->content_type = e->content_type;
	out_view->content_type_len = e->content_type_len;
	out_view->etag = e->etag;
	out_view->etag_len = e->etag_len;
	out_view->last_modified = e->last_modified;
	out_view->last_modified_len = e->last_modified_len;

	return true;
}

void http_static_cache_insert(http_static_cache_t *cache, const char *path, size_t path_len,
							  const zend_stat_t *st, const char *content_type,
							  size_t content_type_len, const char *etag, size_t etag_len,
							  const char *last_modified, size_t last_modified_len)
{
	ZEND_ASSERT(cache != NULL);
	ZEND_ASSERT(path != NULL);
	ZEND_ASSERT(st != NULL);

	if (cache_disabled(cache, path_len)) {
		return;
	}

	/* If the path is already cached (concurrent insert race in a
	 * single-thread loop is impossible, but hot-reload after eviction
	 * can hit this), drop the old entry first to keep the LRU list
	 * in sync. */
	zend_string *key = zend_string_init(path, path_len, 1); /* persistent */
	entry_t *existing = (entry_t *)zend_hash_find_ptr(&cache->index, key);

	if (existing != NULL) {
		evict_entry(cache, existing);
	}

	if (cache->count >= cache->max_entries) {
		evict_lru(cache);
	}

	entry_t *e = pecalloc(1, sizeof(*e), 1);
	e->path_key = key; /* HashTable will release on dtor */
	e->st = *st;
	e->cached_at = time(NULL);

	if (content_type != NULL && content_type_len > 0) {
		e->content_type = pemalloc(content_type_len, 1);
		memcpy(e->content_type, content_type, content_type_len);
		e->content_type_len = content_type_len;
	}

	if (etag != NULL && etag_len > 0) {
		e->etag = pemalloc(etag_len, 1);
		memcpy(e->etag, etag, etag_len);
		e->etag_len = etag_len;
	}

	if (last_modified != NULL && last_modified_len > 0) {
		e->last_modified = pemalloc(last_modified_len, 1);
		memcpy(e->last_modified, last_modified, last_modified_len);
		e->last_modified_len = last_modified_len;
	}

	lru_push_head(cache, e);
	cache->count++;

	/* HashTable takes ownership of the key via the entry pointer
	 * struct. Release of key happens in entry_free via path_key. */
	zend_hash_add_ptr(&cache->index, key, e);
}

/* === Body dedup ================================================== */

static entry_t *find_entry(http_static_cache_t *cache, const char *path, const size_t path_len)
{
	if (cache_disabled(cache, path_len)) {
		return NULL;
	}

	return (entry_t *)zend_hash_str_find_ptr(&cache->index, path, path_len);
}

zend_string *http_static_cache_body_acquire(http_static_cache_t *const cache,
											const char *const path, const size_t path_len)
{
	ZEND_ASSERT(cache != NULL);
	ZEND_ASSERT(path != NULL);

	entry_t *const e = find_entry(cache, path, path_len);

	if (e == NULL || e->body == NULL) {
		return NULL;
	}

	zend_string_addref(e->body);
	return e->body;
}

void http_static_cache_body_store(http_static_cache_t *const cache, const char *const path,
								  const size_t path_len, const char *const body,
								  const size_t body_len)
{
	ZEND_ASSERT(cache != NULL);
	ZEND_ASSERT(body != NULL);

	entry_t *const e = find_entry(cache, path, path_len);

	if (e == NULL || e->body != NULL || body_len == 0) {
		return;
	}

	zend_string *const persistent = zend_string_alloc(body_len, 1);
	memcpy(ZSTR_VAL(persistent), body, body_len);
	ZSTR_VAL(persistent)[body_len] = '\0';
	e->body = persistent;
}

/* === Existence probe ============================================= */

http_static_cache_probe_t http_static_cache_probe(http_static_cache_t *const cache,
												  const char *const path, const size_t path_len)
{
	ZEND_ASSERT(cache != NULL);
	ZEND_ASSERT(path != NULL);

	if (cache_disabled(cache, path_len)) {
		return HTTP_STATIC_CACHE_PROBE_UNKNOWN;
	}

	entry_t *const e = (entry_t *)zend_hash_str_find_ptr(&cache->index, path, path_len);

	if (e == NULL) {
		return HTTP_STATIC_CACHE_PROBE_UNKNOWN;
	}

	/* TTL applies to both positive and negative entries — operator's
	 * "how long am I willing to be stale" knob holds for absence too. */
	if (time(NULL) - e->cached_at > cache->ttl_seconds) {
		evict_entry(cache, e);
		return HTTP_STATIC_CACHE_PROBE_UNKNOWN;
	}

	return e->not_found ? HTTP_STATIC_CACHE_PROBE_NOT_FOUND
						: HTTP_STATIC_CACHE_PROBE_EXISTS;
}

void http_static_cache_negative_insert(http_static_cache_t *const cache,
									   const char *const path, const size_t path_len)
{
	ZEND_ASSERT(cache != NULL);
	ZEND_ASSERT(path != NULL);

	if (cache_disabled(cache, path_len)) {
		return;
	}

	/* If a stale entry already lives here (positive or negative), drop
	 * it first — we're recording fresh evidence. */
	zend_string *key = zend_string_init(path, path_len, 1);
	entry_t *existing = (entry_t *)zend_hash_find_ptr(&cache->index, key);

	if (existing != NULL) {
		evict_entry(cache, existing);
	}

	if (cache->count >= cache->max_entries) {
		evict_lru(cache);
	}

	entry_t *e = pecalloc(1, sizeof(*e), 1);
	e->path_key  = key;
	e->cached_at = time(NULL);
	e->not_found = true;

	lru_push_head(cache, e);
	cache->count++;
	zend_hash_add_ptr(&cache->index, key, e);
}
