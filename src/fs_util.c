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
#include "fs_util.h"

#include <errno.h>
#include <unistd.h>

zend_string *fs_slurp_fd(const int fd, const size_t expected_size)
{
	if (expected_size == 0) {
		return ZSTR_EMPTY_ALLOC();
	}

	zend_string *out = zend_string_alloc(expected_size, 0);
	size_t total = 0;

	while (total < expected_size) {
		const ssize_t n = read(fd, ZSTR_VAL(out) + total, expected_size - total);

		if (EXPECTED(n > 0)) {
			total += (size_t)n;
			continue;
		}

		if (n == 0) {
			break; /* premature EOF */
		}

		if (errno == EINTR) {
			continue;
		}

		zend_string_release(out);
		return NULL;
	}

	if (UNEXPECTED(total != expected_size)) {
		zend_string_release(out);
		return NULL;
	}

	ZSTR_VAL(out)[expected_size] = '\0';
	return out;
}
