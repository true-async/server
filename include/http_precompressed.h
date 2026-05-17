/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

/* Precompressed sidecar resolution.
 *
 * Given a request and a filesystem path, look for sibling files with
 * a `.zst` / `.br` / `.gz` suffix that the client accepts (per its
 * Accept-Encoding header) and rewrite the path to the chosen sidecar.
 *
 * Codec preference is fixed server-side: zstd > brotli > gzip. Which
 * of the three are eligible at all is controlled by the caller via a
 * bitmask, so a static-mount can disable individual codecs while
 * Response::sendFile() can opt in to all three at once. */

#ifndef TRUE_ASYNC_HTTP_PRECOMPRESSED_H
#define TRUE_ASYNC_HTTP_PRECOMPRESSED_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct http_request_t http_request_t;
typedef struct http_static_cache_s http_static_cache_t;

enum {
	HTTP_PRECOMP_BR = 1u << 0,
	HTTP_PRECOMP_GZIP = 1u << 1,
	HTTP_PRECOMP_ZSTD = 1u << 2,
	HTTP_PRECOMP_ALL = HTTP_PRECOMP_BR | HTTP_PRECOMP_GZIP | HTTP_PRECOMP_ZSTD,
};

/* On hit: rewrite `path_buf[0..*path_len)` in place by appending the
 * sidecar suffix, advance `*path_len`, set `*out_encoding` /
 * `*out_encoding_len` to a static codec token (`"zstd"` / `"br"` /
 * `"gzip"`), and return true. On miss the buffer and length are left
 * untouched and the function returns false.
 *
 * `enabled_mask` is a bitwise-OR of HTTP_PRECOMP_* — codecs not in the
 * mask are skipped even if the client accepts them.
 *
 * `cache` is optional; when non-NULL the selector consults the cache's
 * existence probe before stat()ing each candidate, and records negative
 * results so subsequent probes skip the syscall. Positive results are
 * left to the engine's own cache_insert (which carries full metadata).
 * Pass NULL when the caller has no static cache (e.g. Response::sendFile),
 * preserving the original stat-per-codec behaviour. */
bool http_precompressed_select(const http_request_t *request, uint32_t enabled_mask,
							   char *path_buf, size_t buf_cap, size_t *path_len,
							   const char **out_encoding, size_t *out_encoding_len,
							   http_static_cache_t *cache);

#endif
