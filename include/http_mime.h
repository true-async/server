/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

/* Built-in MIME-by-extension lookup. Stateless; binary search over a
 * sorted table of common web extensions. Per-mount overrides are NOT
 * consulted here — callers that need them must check their own
 * override map before falling back to this builtin. */

#ifndef TRUE_ASYNC_HTTP_MIME_H
#define TRUE_ASYNC_HTTP_MIME_H

#include <stdbool.h>
#include <stddef.h>

/* On hit: *out and *out_len point into a process-lifetime literal,
 * returns true. On miss: returns false (caller falls back to
 * "application/octet-stream" or its own override table). */
bool http_mime_lookup_by_ext(const char *path, size_t path_len, const char **out,
							 size_t *out_len);

/* Extract the last dot-extension from `path`, lowercase into `buf`
 * (NUL-terminated). Returns the extension length (without NUL), or 0
 * on no extension / buf too small. Useful for callers that want to
 * key a per-mount override HashTable on the lowered extension before
 * falling back to http_mime_lookup_by_ext. */
size_t http_mime_extract_lowered_ext(const char *path, size_t path_len, char *buf, size_t buf_cap);

#endif
