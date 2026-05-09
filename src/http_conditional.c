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
#include "http_conditional.h"
#include "http_etag.h"
#include "http_date.h"

bool http_conditional_check(const char *if_none_match, size_t if_none_match_len,
							const char *if_modified_since, size_t if_modified_since_len,
							const char *etag, size_t etag_len, time_t last_modified)
{
	/* RFC 9110 §13.1.2: If-None-Match takes precedence; If-Modified-
	 * Since is consulted only when If-None-Match is absent. */
	if (if_none_match_len > 0 && if_none_match != NULL) {
		return http_etag_match_inm(if_none_match, if_none_match_len, etag, etag_len);
	}

	if (if_modified_since_len > 0 && if_modified_since != NULL) {
		const time_t since = http_date_parse_imf(if_modified_since, if_modified_since_len);
		if (since == (time_t)-1) {
			return false;
		}

		/* "Not modified" iff last_modified <= since. */
		return last_modified <= since;
	}

	return false;
}
