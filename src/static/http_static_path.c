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
#include "static/http_static_path.h"

#include <fnmatch.h>
#include <string.h>

/* Returns -1 on non-hex input. */
static inline int hex_value(const char c)
{
	if (c >= '0' && c <= '9') {
		return c - '0';
	}
	if (c >= 'a' && c <= 'f') {
		return 10 + (c - 'a');
	}
	if (c >= 'A' && c <= 'F') {
		return 10 + (c - 'A');
	}
	return -1;
}

/* Decoded literal '/' bytes are preserved — segment-level traversal
 * validation below relies on the separator being unambiguous. NUL
 * bytes (literal or %00) are rejected. */
static http_static_path_result_t percent_decode(const char *const src, const size_t src_len,
												char *const dst, const size_t dst_cap,
												size_t *const dst_len_out)
{
	size_t out = 0;
	for (size_t i = 0; i < src_len;) {
		if (src[i] == '\0') {
			return HTTP_STATIC_PATH_BAD_REQUEST;
		}
		if (src[i] == '%') {
			if (i + 2 >= src_len) {
				return HTTP_STATIC_PATH_BAD_REQUEST;
			}
			const int hi = hex_value(src[i + 1]);
			const int lo = hex_value(src[i + 2]);
			if (hi < 0 || lo < 0) {
				return HTTP_STATIC_PATH_BAD_REQUEST;
			}
			const unsigned char byte = (unsigned char)((hi << 4) | lo);
			/* NUL ends C strings; backslash is a path separator on
			 * Windows and would bypass the segment validator below. */
			if (byte == 0 || byte == '\\') {
				return HTTP_STATIC_PATH_BAD_REQUEST;
			}
			if (out + 1 >= dst_cap) {
				return HTTP_STATIC_PATH_BAD_REQUEST;
			}
			dst[out++] = (char)byte;
			i += 3;
			continue;
		}
		if (UNEXPECTED(src[i] == '\\')) {
			return HTTP_STATIC_PATH_BAD_REQUEST;
		}
		if (out + 1 >= dst_cap) {
			return HTTP_STATIC_PATH_BAD_REQUEST;
		}
		dst[out++] = src[i++];
	}
	dst[out] = '\0';
	*dst_len_out = out;
	return HTTP_STATIC_PATH_OK;
}

/* DoS guard: cap how many path segments we are willing to walk. A
 * malicious request like "/a/a/a/..." with 100k segments forces N
 * lstat() calls in OWNER mode and N hash lookups in REJECT mode.
 * 256 is comfortably above any legitimate filesystem depth (Linux
 * PATH_MAX of 4096 bytes already bounds total length, this caps
 * fan-out). Tune if a real workload demands more. */
#define HTTP_STATIC_PATH_MAX_SEGMENTS 256

/* Reject empty / "." / ".." segments and apply the dotfile policy.
 * `path` always starts with '/' so the leading separator is skipped. */
static http_static_path_result_t validate_segments(const http_static_handler_t *mount,
												   const char *const path, const size_t path_len)
{
	size_t i = (path_len > 0 && path[0] == '/') ? 1 : 0;
	size_t segment_count = 0;

	while (i < path_len) {
		if (UNEXPECTED(++segment_count > HTTP_STATIC_PATH_MAX_SEGMENTS)) {
			return HTTP_STATIC_PATH_BAD_REQUEST;
		}
		const size_t seg_start = i;
		while (i < path_len && path[i] != '/') {
			i++;
		}
		const size_t seg_len = i - seg_start;
		const char *const seg = path + seg_start;

		if (seg_len == 0) {
			return HTTP_STATIC_PATH_BAD_REQUEST;
		}
		if (seg_len == 1 && seg[0] == '.') {
			return HTTP_STATIC_PATH_FORBIDDEN;
		}
		if (seg_len == 2 && seg[0] == '.' && seg[1] == '.') {
			return HTTP_STATIC_PATH_FORBIDDEN;
		}
		if (seg[0] == '.') {
			if (mount->flags & HTTP_STATIC_FLAG_DOTFILES_DENY) {
				return HTTP_STATIC_PATH_FORBIDDEN;
			}
			if (mount->flags & HTTP_STATIC_FLAG_DOTFILES_IGNORE) {
				return HTTP_STATIC_PATH_HIDE;
			}
			/* DOTFILES_ALLOW falls through. */
		}
		if (i < path_len) {
			i++;
		}
	}

	return HTTP_STATIC_PATH_OK;
}

http_static_path_result_t
http_static_path_resolve(const http_static_handler_t *mount, const char *request_path,
						 size_t request_path_len, char *out_buf, size_t out_buf_cap,
						 size_t *out_len, const char **out_relative, size_t *out_relative_len)
{
	if (UNEXPECTED(mount == NULL || mount->url_prefix == NULL || mount->root_directory == NULL)) {
		return HTTP_STATIC_PATH_NO_MATCH;
	}
	/* Defensive: upstream parsers should already reject absolute-form
	 * / authority-form / asterisk targets. */
	if (UNEXPECTED(request_path_len == 0 || request_path[0] != '/')) {
		return HTTP_STATIC_PATH_BAD_REQUEST;
	}

	const size_t prefix_len = mount->url_prefix_len;
	if (request_path_len < prefix_len) {
		return HTTP_STATIC_PATH_NO_MATCH;
	}
	if (memcmp(request_path, ZSTR_VAL(mount->url_prefix), prefix_len) != 0) {
		return HTTP_STATIC_PATH_NO_MATCH;
	}

	const char *const tail = request_path + prefix_len;
	size_t tail_len = request_path_len - prefix_len;

	/* Defensive: llhttp hands us path-only, H2/H3 paths may differ. */
	for (size_t i = 0; i < tail_len; i++) {
		if (tail[i] == '?' || tail[i] == '#') {
			tail_len = i;
			break;
		}
	}

	char decoded[MAXPATHLEN];
	size_t decoded_len = 0;
	const http_static_path_result_t decode_rc =
		percent_decode(tail, tail_len, decoded, sizeof(decoded), &decoded_len);
	if (UNEXPECTED(decode_rc != HTTP_STATIC_PATH_OK)) {
		return decode_rc;
	}

	/* Empty tail (URL == prefix) is allowed — caller tries the index
	 * files. Otherwise validate the segment grammar. validate_segments
	 * tolerates either a leading '/' or no leading '/' (it skips one
	 * if present), so feeding `decoded` directly avoids a 4 KiB stack
	 * scratch + a memcpy on every request. */
	if (decoded_len > 0) {
		const http_static_path_result_t seg_rc = validate_segments(mount, decoded, decoded_len);
		if (UNEXPECTED(seg_rc != HTTP_STATIC_PATH_OK)) {
			return seg_rc;
		}
	}

	/* realpath()-canonical root has no trailing '/' except when the
	 * root is exactly "/". */
	const char *const root = ZSTR_VAL(mount->root_directory);
	const size_t root_len = ZSTR_LEN(mount->root_directory);

	if (UNEXPECTED(root_len + 1 + decoded_len + 1 > out_buf_cap)) {
		return HTTP_STATIC_PATH_BAD_REQUEST;
	}
	memcpy(out_buf, root, root_len);
	size_t out = root_len;
	if (out > 0 && out_buf[out - 1] != '/') {
		out_buf[out++] = '/';
	}
	memcpy(out_buf + out, decoded, decoded_len);
	out_buf[out + decoded_len] = '\0';
	*out_len = out + decoded_len;

	if (out_relative != NULL) {
		*out_relative = out_buf + out;
		*out_relative_len = decoded_len;
	}

	return HTTP_STATIC_PATH_OK;
}

bool http_static_path_join(char *const buf, const size_t cap, size_t *const len,
						   const char *const name, const size_t name_len)
{
	size_t cur = *len;
	const bool need_sep = (cur == 0 || buf[cur - 1] != '/');
	const size_t extra = (need_sep ? 1 : 0) + name_len + 1;
	if (cur + extra > cap) {
		return false;
	}
	if (need_sep) {
		buf[cur++] = '/';
	}
	memcpy(buf + cur, name, name_len);
	cur += name_len;
	buf[cur] = '\0';
	*len = cur;
	return true;
}

bool http_static_path_is_hidden(const http_static_handler_t *mount, const char *relative,
								size_t relative_len)
{
	if (mount == NULL || mount->hide_count == 0) {
		return false;
	}
	/* http_static_path_resolve writes a NUL right after `relative`
	 * inside the caller's out_buf (out_buf[out + decoded_len] = '\0'),
	 * so `relative[relative_len] == '\0'` already.  Pass it straight
	 * to fnmatch and skip the per-request memcpy + 4 KiB scratch. */
	(void)relative_len;
	for (size_t i = 0; i < mount->hide_count; i++) {
		const zend_string *const glob = mount->hide_globs[i];
		if (fnmatch(ZSTR_VAL(glob), relative, FNM_PATHNAME) == 0) {
			return true;
		}
	}
	return false;
}
