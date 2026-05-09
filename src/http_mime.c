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
#include "http_mime.h"

#include <ctype.h>
#include <string.h>

/* Lowercase ASCII keys, sorted lexicographically — lookup is a
 * binary search and the candidate extension is lowercased into a
 * stack buffer before comparing. KEEP SORTED ON INSERT: a debug-build
 * runtime check at first lookup catches ordering regressions. */
typedef struct
{
	const char *extension;
	const char *content_type;
	size_t content_type_len; /* precomputed; saves a strlen per hit */
} http_mime_entry_t;

#define MIME(ext, ct)           \
	{                           \
		ext, ct, sizeof(ct) - 1 \
	}

static const http_mime_entry_t builtin_table[] = {
	MIME("atom", "application/atom+xml"),
	MIME("avif", "image/avif"),
	MIME("bin", "application/octet-stream"),
	MIME("bmp", "image/bmp"),
	MIME("css", "text/css; charset=utf-8"),
	MIME("csv", "text/csv; charset=utf-8"),
	MIME("eot", "application/vnd.ms-fontobject"),
	MIME("gif", "image/gif"),
	MIME("gz", "application/gzip"),
	MIME("htm", "text/html; charset=utf-8"),
	MIME("html", "text/html; charset=utf-8"),
	MIME("ico", "image/x-icon"),
	MIME("jpeg", "image/jpeg"),
	MIME("jpg", "image/jpeg"),
	MIME("js", "text/javascript; charset=utf-8"),
	MIME("json", "application/json"),
	MIME("manifest", "text/cache-manifest"),
	MIME("map", "application/json"),
	MIME("md", "text/markdown; charset=utf-8"),
	MIME("mjs", "text/javascript; charset=utf-8"),
	MIME("mp3", "audio/mpeg"),
	MIME("mp4", "video/mp4"),
	MIME("ogg", "audio/ogg"),
	MIME("otf", "font/otf"),
	MIME("pdf", "application/pdf"),
	MIME("png", "image/png"),
	MIME("rss", "application/rss+xml"),
	MIME("svg", "image/svg+xml"),
	MIME("tar", "application/x-tar"),
	MIME("tif", "image/tiff"),
	MIME("tiff", "image/tiff"),
	MIME("ttf", "font/ttf"),
	MIME("txt", "text/plain; charset=utf-8"),
	MIME("wasm", "application/wasm"),
	MIME("wav", "audio/wav"),
	MIME("webm", "video/webm"),
	MIME("webmanifest", "application/manifest+json"),
	MIME("webp", "image/webp"),
	MIME("woff", "font/woff"),
	MIME("woff2", "font/woff2"),
	MIME("xhtml", "application/xhtml+xml"),
	MIME("xml", "text/xml; charset=utf-8"),
	MIME("yaml", "application/yaml"),
	MIME("yml", "application/yaml"),
	MIME("zip", "application/zip"),
	MIME("zst", "application/zstd"),
};

#undef MIME

#define BUILTIN_TABLE_LEN (sizeof(builtin_table) / sizeof(builtin_table[0]))

/* Offset just past the last '.' in `path`, or path_len when no
 * extension is present. Bails out at any path separator so a name
 * like "foo.tar/x" can't leak the parent dir's extension. On Windows
 * both '/' and '\\' are separators (php_basename / IS_SLASH semantics).
 */
static inline bool is_path_separator(const char c)
{
#ifdef PHP_WIN32
	return c == '/' || c == '\\';
#else
	return c == '/';
#endif
}

static inline size_t find_extension_offset(const char *path, const size_t path_len)
{
	size_t i = path_len;
	while (i > 0) {
		const char c = path[i - 1];

		if (is_path_separator(c)) {
			return path_len;
		}

		if (c == '.') {
			return i;
		}

		i--;
	}

	return path_len;
}

/* Returns 0 on empty / overflow. */
static inline size_t lower_extension(const char *src, const size_t src_len, char *buf,
									 const size_t buf_cap)
{
	if (src_len == 0 || src_len >= buf_cap) {
		return 0;
	}

	for (size_t i = 0; i < src_len; i++) {
		char c = src[i];

		if (c >= 'A' && c <= 'Z') {
			c = (char)(c - 'A' + 'a');
		}

		buf[i] = c;
	}

	buf[src_len] = '\0';
	return src_len;
}

static const http_mime_entry_t *lookup_builtin(const char *ext)
{
	size_t lo = 0;
	size_t hi = BUILTIN_TABLE_LEN;
	while (lo < hi) {
		const size_t mid = lo + (hi - lo) / 2;
		const int cmp = strcmp(ext, builtin_table[mid].extension);

		if (cmp == 0) {
			return &builtin_table[mid];
		}

		if (cmp < 0) {
			hi = mid;
		} else {
			lo = mid + 1;
		}
	}

	return NULL;
}

#ifndef NDEBUG
/* Debug-build safety net: catches a misordered table addition the
 * first time the lookup runs. Cost is one strcmp per built-in entry,
 * once per process. */
static void assert_builtin_table_sorted(void)
{
	static bool checked = false;

	if (checked) {
		return;
	}

	for (size_t i = 1; i < BUILTIN_TABLE_LEN; i++) {
		ZEND_ASSERT(strcmp(builtin_table[i - 1].extension, builtin_table[i].extension) < 0);
	}

	checked = true;
}
#endif

size_t http_mime_extract_lowered_ext(const char *path, size_t path_len, char *buf, size_t buf_cap)
{
	const size_t ext_offset = find_extension_offset(path, path_len);

	if (ext_offset >= path_len) {
		return 0;
	}

	return lower_extension(path + ext_offset, path_len - ext_offset, buf, buf_cap);
}

bool http_mime_lookup_by_ext(const char *path, size_t path_len, const char **out, size_t *out_len)
{
#ifndef NDEBUG
	assert_builtin_table_sorted();
#endif

	const size_t ext_offset = find_extension_offset(path, path_len);

	if (ext_offset >= path_len) {
		return false;
	}

	const char *ext_src = path + ext_offset;
	const size_t ext_src_len = path_len - ext_offset;

	char ext_buf[32];
	const size_t ext_len = lower_extension(ext_src, ext_src_len, ext_buf, sizeof(ext_buf));

	if (UNEXPECTED(ext_len == 0)) {
		return false;
	}

	const http_mime_entry_t *hit = lookup_builtin(ext_buf);

	if (hit != NULL) {
		*out = hit->content_type;
		*out_len = hit->content_type_len;
		return true;
	}

	return false;
}
