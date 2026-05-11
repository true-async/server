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

#include <sys/param.h>
#include <sys/stat.h>
#include <string.h>

bool http_precompressed_select(const http_request_t *request, const uint32_t enabled_mask,
							   char *path_buf, const size_t buf_cap, size_t *path_len,
							   const char **out_encoding, size_t *out_encoding_len)
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
		candidate[*path_len + codecs[i].suffix_len] = '\0';

		struct stat st;

		if (stat(candidate, &st) != 0 || !S_ISREG(st.st_mode)) {
			continue;
		}

		memcpy(path_buf + *path_len, codecs[i].suffix, codecs[i].suffix_len);
		*path_len += codecs[i].suffix_len;
		path_buf[*path_len] = '\0';
		*out_encoding = codecs[i].token;
		*out_encoding_len = codecs[i].token_len;
		return true;
	}

	return false;
}
