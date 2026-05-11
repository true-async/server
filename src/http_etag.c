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
#include "http_etag.h"

#include <string.h>

static inline uint64_t mtime_ns_from_stat(const struct stat *st)
{
#if defined(__APPLE__)
	return (uint64_t)st->st_mtimespec.tv_sec * 1000000000ULL + (uint64_t)st->st_mtimespec.tv_nsec;
#elif defined(_WIN32)
	return (uint64_t)st->st_mtime * 1000000000ULL;
#else
	return (uint64_t)st->st_mtim.tv_sec * 1000000000ULL + (uint64_t)st->st_mtim.tv_nsec;
#endif
}

static const char hex_lower[16] = {'0', '1', '2', '3', '4', '5', '6', '7',
								   '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};

static inline void put_u64_16hex(char *out, uint64_t v)
{
	for (int i = 15; i >= 0; --i) {
		out[i] = hex_lower[v & 0xF];
		v >>= 4;
	}
}

void http_etag_format_strong(const struct stat *st, char buf[HTTP_ETAG_BUF_LEN])
{
	/* Single 64-bit mix — SHA/MD5 cost more per request than the
	 * conditional GET saves. See docs/PLAN_STATIC_HANDLER.md §4. */
	const uint64_t mtime_ns = mtime_ns_from_stat(st);
	const uint64_t size = (uint64_t)st->st_size;
	const uint64_t ino = (uint64_t)st->st_ino;
	const uint64_t mix = mtime_ns ^ (size << 17) ^ ino;

	/* Layout: W / " <16-hex> " — 20 bytes + NUL. */
	buf[0] = 'W';
	buf[1] = '/';
	buf[2] = '"';
	put_u64_16hex(buf + 3, mix);
	buf[19] = '"';
	buf[20] = '\0';
	ZEND_STATIC_ASSERT(HTTP_ETAG_LEN == 20, "etag length must match hand-format");
}

static inline void trim_ws(const char **start, size_t *len)
{
	while (*len > 0 && ((*start)[0] == ' ' || (*start)[0] == '\t')) {
		(*start)++;
		(*len)--;
	}
	while (*len > 0 && ((*start)[*len - 1] == ' ' || (*start)[*len - 1] == '\t')) {
		(*len)--;
	}
}

/* Strip a leading W/ (case-sensitive per RFC 9110 §8.8.3) so a weak
 * tag from the server compares equal to a weak tag echoed by the
 * client. */
static inline void strip_weak_prefix(const char **s, size_t *len)
{
	if (*len >= 2 && (*s)[0] == 'W' && (*s)[1] == '/') {
		*s += 2;
		*len -= 2;
	}
}

bool http_etag_match_inm(const char *header, size_t header_len, const char *etag, size_t etag_len)
{
	/* Strip W/ on the etag side too so a strong tag compared to a
	 * weak echo matches "weak-equal" as RFC 9110 §13.1.2 prescribes
	 * for If-None-Match. */
	strip_weak_prefix(&etag, &etag_len);

	size_t i = 0;
	while (i < header_len) {
		size_t start = i;
		while (i < header_len && header[i] != ',') {
			i++;
		}

		size_t entry_len = i - start;
		const char *entry = header + start;
		trim_ws(&entry, &entry_len);

		if (entry_len == 1 && entry[0] == '*') {
			return true;
		}

		const char *cmp = entry;
		size_t cmp_len = entry_len;
		strip_weak_prefix(&cmp, &cmp_len);

		if (cmp_len == etag_len && memcmp(cmp, etag, etag_len) == 0) {
			return true;
		}

		if (i < header_len) {
			i++; /* skip the comma */
		}
	}

	return false;
}
