/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include "php.h"
#include "static/http_static_mime.h"

#include <ctype.h>
#include <string.h>

/* Built-in extension → Content-Type table. SORTED by extension —
 * binary search hits each entry in log2(N) compares, no hashing. The
 * set targets the HttpArena static profile plus the long-tail of
 * everyday web content; anything outside this list either falls
 * through to a per-mount override or to application/octet-stream.
 *
 * Entries should be lowercase ASCII; lookup lowercases the candidate
 * extension before searching. Keep sorted on insert — a static_assert
 * verifies ordering at first lookup. */
typedef struct {
    const char *extension;
    const char *content_type;
} http_static_mime_entry_t;

static const http_static_mime_entry_t builtin_table[] = {
    /* sorted lexicographically */
    { "atom",  "application/atom+xml" },
    { "avif",  "image/avif" },
    { "bin",   "application/octet-stream" },
    { "bmp",   "image/bmp" },
    { "css",   "text/css; charset=utf-8" },
    { "csv",   "text/csv; charset=utf-8" },
    { "eot",   "application/vnd.ms-fontobject" },
    { "gif",   "image/gif" },
    { "gz",    "application/gzip" },
    { "htm",   "text/html; charset=utf-8" },
    { "html",  "text/html; charset=utf-8" },
    { "ico",   "image/x-icon" },
    { "jpeg",  "image/jpeg" },
    { "jpg",   "image/jpeg" },
    { "js",    "text/javascript; charset=utf-8" },
    { "json",  "application/json" },
    { "manifest", "text/cache-manifest" },
    { "map",   "application/json" },
    { "md",    "text/markdown; charset=utf-8" },
    { "mjs",   "text/javascript; charset=utf-8" },
    { "mp3",   "audio/mpeg" },
    { "mp4",   "video/mp4" },
    { "ogg",   "audio/ogg" },
    { "otf",   "font/otf" },
    { "pdf",   "application/pdf" },
    { "png",   "image/png" },
    { "rss",   "application/rss+xml" },
    { "svg",   "image/svg+xml" },
    { "tar",   "application/x-tar" },
    { "tif",   "image/tiff" },
    { "tiff",  "image/tiff" },
    { "ttf",   "font/ttf" },
    { "txt",   "text/plain; charset=utf-8" },
    { "wasm",  "application/wasm" },
    { "wav",   "audio/wav" },
    { "webm",  "video/webm" },
    { "webmanifest", "application/manifest+json" },
    { "webp",  "image/webp" },
    { "woff",  "font/woff" },
    { "woff2", "font/woff2" },
    { "xhtml", "application/xhtml+xml" },
    { "xml",   "text/xml; charset=utf-8" },
    { "yaml",  "application/yaml" },
    { "yml",   "application/yaml" },
    { "zip",   "application/zip" },
    { "zst",   "application/zstd" },
};

#define BUILTIN_TABLE_LEN \
    (sizeof(builtin_table) / sizeof(builtin_table[0]))

/* Returns the byte offset of the extension after the final '.' in
 * `path`, or path_len when there is no extension. The extension is
 * everything between the last '.' and the end of the path, exclusive
 * of the dot itself. Bails out at any '/' so a name like "foo.tar/x"
 * (impossible after canonicalisation, but cheap to check) does not
 * leak the parent dir's extension. */
static inline size_t find_extension_offset(const char *path, size_t path_len)
{
    size_t i = path_len;
    while (i > 0) {
        const char c = path[i - 1];
        if (c == '/') return path_len;
        if (c == '.') return i;
        i--;
    }
    return path_len;
}

/* Lowercase-copy the extension into `buf`. Returns the number of
 * bytes written, or 0 on overflow / empty extension. ASCII only —
 * extensions outside ASCII would be a configuration smell. */
static inline size_t lower_extension(const char *src, size_t src_len,
                                     char *buf, size_t buf_cap)
{
    if (src_len == 0 || src_len >= buf_cap) {
        return 0;
    }
    for (size_t i = 0; i < src_len; i++) {
        char c = src[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        buf[i] = c;
    }
    buf[src_len] = '\0';
    return src_len;
}

/* Binary search the sorted built-in table. */
static const http_static_mime_entry_t *
lookup_builtin(const char *ext, size_t ext_len)
{
    (void)ext_len;
    size_t lo = 0;
    size_t hi = BUILTIN_TABLE_LEN;
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        const int cmp = strcmp(ext, builtin_table[mid].extension);
        if (cmp == 0) return &builtin_table[mid];
        if (cmp < 0) hi = mid;
        else         lo = mid + 1;
    }
    return NULL;
}

bool http_static_mime_lookup(const http_static_handler_t *mount,
                             const char *path, size_t path_len,
                             const char **out, size_t *out_len)
{
    const size_t ext_offset = find_extension_offset(path, path_len);
    if (ext_offset >= path_len) {
        return false;
    }

    const char *ext_src = path + ext_offset;
    const size_t ext_src_len = path_len - ext_offset;

    char ext_buf[32];
    const size_t ext_len = lower_extension(ext_src, ext_src_len,
                                           ext_buf, sizeof(ext_buf));
    if (ext_len == 0) {
        return false;
    }

    /* Per-mount overrides win over the built-in table. The plan §1
     * setMimeType is documented as "override the Content-Type for
     * files with the given extension" — that obligates override > base. */
    if (mount != NULL && mount->mime_overrides != NULL) {
        zval *override = zend_hash_str_find(mount->mime_overrides,
                                            ext_buf, ext_len);
        if (override != NULL && Z_TYPE_P(override) == IS_STRING) {
            *out     = Z_STRVAL_P(override);
            *out_len = Z_STRLEN_P(override);
            return true;
        }
    }

    const http_static_mime_entry_t *hit = lookup_builtin(ext_buf, ext_len);
    if (hit != NULL) {
        *out     = hit->content_type;
        *out_len = strlen(hit->content_type);
        return true;
    }

    return false;
}
