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

#include "http_param_parse.h"

#include <stddef.h>

static inline bool at_end(const char *p, const char *end)
{
	if (end != NULL) {
		return p >= end;
	}

	return *p == '\0';
}

static inline bool is_ws(const char c)
{
	return c == ' ' || c == '\t';
}

bool http_header_param_next(const char **cursor, const char *end, http_param_t *out)
{
	const char *p = *cursor;

	/* Skip leading separators and whitespace. */
	while (!at_end(p, end) && (*p == ';' || is_ws(*p))) {
		p++;
	}

	if (at_end(p, end)) {
		*cursor = p;
		return false;
	}

	/* Parameter name — up to '=', ';', or whitespace. */
	const char *name_start = p;

	while (!at_end(p, end) && *p != '=' && *p != ';' && !is_ws(*p)) {
		p++;
	}

	out->name = name_start;
	out->name_len = (size_t)(p - name_start);

	/* No '=' → value-less token (e.g., bare "form-data"). Skip to next ';'. */
	if (at_end(p, end) || *p != '=') {
		while (!at_end(p, end) && *p != ';') {
			p++;
		}

		out->value = NULL;
		out->value_len = 0;
		out->quoted = false;
		*cursor = p;
		return out->name_len > 0;
	}

	p++; /* past '=' */

	if (!at_end(p, end) && *p == '"') {
		p++;
		const char *val_start = p;

		while (!at_end(p, end) && *p != '"') {
			p++;
		}

		out->value = val_start;
		out->value_len = (size_t)(p - val_start);
		out->quoted = true;

		if (!at_end(p, end) && *p == '"') {
			p++;
		}
	} else {
		const char *val_start = p;

		while (!at_end(p, end) && *p != ';' && !is_ws(*p)) {
			p++;
		}

		out->value = val_start;
		out->value_len = (size_t)(p - val_start);
		out->quoted = false;
	}

	/* Skip trailing junk up to next ';'. */
	while (!at_end(p, end) && *p != ';') {
		p++;
	}

	*cursor = p;
	return true;
}
