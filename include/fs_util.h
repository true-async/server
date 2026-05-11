/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

/* Synchronous filesystem helpers used by the small-file fallback paths. */

#ifndef TRUE_ASYNC_FS_UTIL_H
#define TRUE_ASYNC_FS_UTIL_H

#include "Zend/zend_string.h"
#include <stddef.h>

/* Read exactly `expected_size` bytes from `fd` into a freshly-allocated
 * zend_string. Retries EINTR. Returns NULL on any IO error or on
 * premature EOF. */
zend_string *fs_slurp_fd(int fd, size_t expected_size);

#endif
