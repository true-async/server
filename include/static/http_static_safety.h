/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

/* Path-safety gates for static-handler dispatch.
 *
 * Three checks layered on top of the regular open(2):
 *   - symlink_policy_admits: lstat-walk against mount->flags symlink
 *                             policy (REJECT / OWNER_MATCH / FOLLOW).
 *   - resolved_under_root:   realpath-based prefix check guarding
 *                             against intermediate-symlink escapes.
 *   - try_open_candidate:    open + fstat + lstat re-validation; closes
 *                             the §13d TOCTOU window between the
 *                             policy lstat-walk and the actual open(2).
 *
 * All three operate on `const http_static_handler_t *mount` (read-only
 * mount descriptor). The functions are protocol-agnostic and have no
 * side effects beyond the requested filesystem syscalls. */

#ifndef TRUE_ASYNC_HTTP_STATIC_SAFETY_H
#define TRUE_ASYNC_HTTP_STATIC_SAFETY_H

#include <stdbool.h>
#include <sys/stat.h>

#include "static/static_handler.h"

bool http_static_symlink_policy_admits(const http_static_handler_t *mount, const char *fs_path);

bool http_static_resolved_under_root(const http_static_handler_t *mount, const char *path);

bool http_static_try_open_candidate(const http_static_handler_t *mount, const char *path,
									int *out_fd, struct stat *st);

#endif
