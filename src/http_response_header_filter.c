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

#include "http_response_header_filter.h"

#include <strings.h>

bool http_response_header_allowed_h2h3(const char *name, const size_t len)
{
	if (len == 10 && strncasecmp(name, "connection", 10) == 0) {
		return false;
	}

	if (len == 10 && strncasecmp(name, "keep-alive", 10) == 0) {
		return false;
	}

	if (len == 17 && strncasecmp(name, "transfer-encoding", 17) == 0) {
		return false;
	}

	if (len == 7 && strncasecmp(name, "upgrade", 7) == 0) {
		return false;
	}

	/* content-length is implicit from the DATA frames. */
	if (len == 14 && strncasecmp(name, "content-length", 14) == 0) {
		return false;
	}

	return true;
}
