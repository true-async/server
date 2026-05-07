/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

/* URL → filesystem path translation for the static handler.
 *
 * Pure validation: percent-decode + traversal guard + dotfile check
 * + concatenation against the canonical root. The result is a NUL-
 * terminated absolute path that has been syntactically validated; the
 * caller still has to open(2) it (which catches symlinks via O_NOFOLLOW
 * if requested, and TOCTOU is handled by realpath()-on-fd in the FSM
 * later). */

#ifndef TRUE_ASYNC_HTTP_STATIC_PATH_H
#define TRUE_ASYNC_HTTP_STATIC_PATH_H

#include "static/static_handler.h"
#include <stdbool.h>
#include <stddef.h>

typedef enum {
    HTTP_STATIC_PATH_OK             = 0,
    HTTP_STATIC_PATH_BAD_REQUEST    = 1,  /* 400: malformed encoding, NUL, etc. */
    HTTP_STATIC_PATH_FORBIDDEN      = 2,  /* 403: traversal escapes root, dotfile deny */
    HTTP_STATIC_PATH_HIDE           = 3,  /* matched a hide glob → 404 (or passthrough) */
    HTTP_STATIC_PATH_NO_MATCH       = 4,  /* prefix didn't match this mount */
} http_static_path_result_t;

/* Resolve `request_path` against `mount`. On HTTP_STATIC_PATH_OK,
 * `out_buf` (a caller-owned buffer of at least `out_buf_cap` bytes)
 * is populated with a NUL-terminated absolute path inside
 * `mount->root_directory`. `*out_len` carries the byte length, not
 * counting the NUL.
 *
 * The relative remainder (the part of the URL path that landed under
 * the root) is also reported via `*out_relative` / `*out_relative_len`,
 * pointing INTO `out_buf` after `mount->root_directory`. Useful for
 * hide-glob matching and for the index-file directory join. */
http_static_path_result_t
http_static_path_resolve(const http_static_handler_t *mount,
                         const char *request_path, size_t request_path_len,
                         char *out_buf, size_t out_buf_cap, size_t *out_len,
                         const char **out_relative, size_t *out_relative_len);

/* Append `name` to an already-resolved directory path stored in
 * `buf` (with current length `*len`). Returns false on overflow.
 * Used by the index-file resolution loop. */
bool http_static_path_join(char *buf, size_t cap, size_t *len,
                           const char *name, size_t name_len);

/* Returns true when the relative path matches one of the mount's
 * hide globs. Uses fnmatch-style matching (FNM_PATHNAME so '/'
 * doesn't match through '*'). */
bool http_static_path_is_hidden(const http_static_handler_t *mount,
                                const char *relative, size_t relative_len);

#endif
