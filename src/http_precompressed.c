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
#include "php_http_server.h"
#include "http_precompressed.h"
#include "compression/http_compression_negotiate.h"
#include "static/http_static_cache.h"

#include "Zend/zend_stream.h"    /* zend_stat_t */
#include "Zend/zend_virtual_cwd.h" /* VCWD_STAT */
#include <string.h>

/* sys/param.h (POSIX) is the usual source of MAXPATHLEN; php.h defines
 * it on all platforms via main/php.h. Provide a fallback just in case. */
#ifndef MAXPATHLEN
# define MAXPATHLEN 4096
#endif

/* POSIX S_ISREG is absent from some Windows CRT headers; define it if
 * the platform does not supply it (php.h win32/param.h usually does). */
#ifndef S_ISREG
# define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#endif

bool http_precompressed_select(const http_request_t *request, const uint32_t enabled_mask,
							   char *path_buf, const size_t buf_cap, size_t *path_len,
							   const char **out_encoding, size_t *out_encoding_len,
							   http_static_cache_t *const cache)
{
	if (enabled_mask == 0) {
		return false;
	}

	const zend_string *ae = http_request_find_header(request, "accept-encoding", 15);

	if (ae == NULL) {
		return false;
	}

	http_accept_encoding_t parsed;
	http_accept_encoding_parse(ZSTR_VAL(ae), ZSTR_LEN(ae), &parsed);

	/* Server preference: zstd > brotli > gzip. identity_acceptable
	 * doesn't gate us — the original file is always available as the
	 * identity fallback if no sidecar matches. */
	static const struct
	{
		uint32_t flag;
		const char *suffix;
		size_t suffix_len;
		const char *token;
		size_t token_len;
	} codecs[] = {
		{HTTP_PRECOMP_ZSTD, ".zst", 4, "zstd", 4},
		{HTTP_PRECOMP_BR, ".br", 3, "br", 2},
		{HTTP_PRECOMP_GZIP, ".gz", 3, "gzip", 4},
	};
	const bool acceptable[] = {
		parsed.zstd_acceptable,
		parsed.brotli_acceptable,
		parsed.gzip_acceptable,
	};

	for (size_t i = 0; i < sizeof(codecs) / sizeof(codecs[0]); i++) {
		if ((enabled_mask & codecs[i].flag) == 0) {
			continue;
		}

		if (!acceptable[i]) {
			continue;
		}

		if (UNEXPECTED(*path_len + codecs[i].suffix_len + 1 > buf_cap)) {
			continue;
		}

		char candidate[MAXPATHLEN];
		memcpy(candidate, path_buf, *path_len);
		memcpy(candidate + *path_len, codecs[i].suffix, codecs[i].suffix_len);
		const size_t candidate_len = *path_len + codecs[i].suffix_len;
		candidate[candidate_len] = '\0';

		/* Cache probe (nginx open_file_cache analogue). Positive entries
		 * are seeded by the engine's cache_insert after a real serve;
		 * negative entries are seeded just below. Either way, a probe
		 * hit lets us skip the stat. */
		if (cache != NULL) {
			const http_static_cache_probe_t pr =
				http_static_cache_probe(cache, candidate, candidate_len);

			if (pr == HTTP_STATIC_CACHE_PROBE_NOT_FOUND) {
				continue;
			}

			if (pr == HTTP_STATIC_CACHE_PROBE_EXISTS) {
				goto take;
			}
			/* UNKNOWN — fall through to stat. */
		}

		zend_stat_t st;

		if (VCWD_STAT(candidate, &st) != 0 || !S_ISREG(st.st_mode)) {
			if (cache != NULL) {
				http_static_cache_negative_insert(cache, candidate, candidate_len);
			}

			continue;
		}
		/* Positive existence — let the engine's cache_insert publish
		 * the full metadata entry later. We don't seed a stub here so
		 * the entry that lands carries real etag/MIME/last-modified. */

take:
		memcpy(path_buf + *path_len, codecs[i].suffix, codecs[i].suffix_len);
		*path_len = candidate_len;
		path_buf[*path_len] = '\0';
		*out_encoding = codecs[i].token;
		*out_encoding_len = codecs[i].token_len;
		return true;
	}

	return false;
}
