/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

/* Built-in MIME lookup. Hot path: a sorted-array binary search over
 * the HttpArena set; per-mount overrides are consulted only on a miss
 * via the embedded HashTable on the descriptor. No allocations on the
 * hit path. */

#ifndef TRUE_ASYNC_HTTP_STATIC_MIME_H
#define TRUE_ASYNC_HTTP_STATIC_MIME_H

#include "static/static_handler.h"
#include <stddef.h>

/* Resolve a Content-Type value for the file at `path` (NUL-terminated,
 * already canonicalised). Returns a `const char *` literal lifetime
 * string from the built-in table on a hit, or a `zend_string *`-backed
 * value from the per-mount override HashTable.
 *
 * `*out` is set to the content-type. `*out_len` is its byte length.
 * Returns true on hit, false when nothing matched (caller falls back
 * to "application/octet-stream"). */
bool http_static_mime_lookup(const http_static_handler_t *mount, const char *path, size_t path_len,
							 const char **out, size_t *out_len);

#endif
