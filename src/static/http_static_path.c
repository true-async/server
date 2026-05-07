/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include "php.h"
#include "static/http_static_path.h"

#include <fnmatch.h>
#include <string.h>

/* Hex-decode one byte of a percent-encoded triplet. Returns -1 on
 * invalid hex. */
static inline int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

/* Percent-decode `src` into `dst`. NUL bytes are rejected with the
 * BAD_REQUEST result. Bytes that are NOT percent-encoded are copied
 * verbatim — including any literal '/' which is the path separator
 * we still need for segment-level traversal checks below. Never
 * over-runs `dst_cap`. */
static http_static_path_result_t
percent_decode(const char *src, size_t src_len,
               char *dst, size_t dst_cap, size_t *dst_len_out)
{
    size_t out = 0;
    for (size_t i = 0; i < src_len; ) {
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
            if (byte == 0) {
                return HTTP_STATIC_PATH_BAD_REQUEST;
            }
            if (out + 1 >= dst_cap) {
                return HTTP_STATIC_PATH_BAD_REQUEST;
            }
            dst[out++] = (char)byte;
            i += 3;
            continue;
        }
        if (out + 1 >= dst_cap) {
            return HTTP_STATIC_PATH_BAD_REQUEST;
        }
        dst[out++] = src[i++];
    }
    dst[out]      = '\0';
    *dst_len_out  = out;
    return HTTP_STATIC_PATH_OK;
}

/* Walk segments of `path` (length `path_len`), enforcing:
 *   - no empty segment after the first character (rejects "//");
 *   - no segment equal to "." or "..";
 *   - dotfile policy on segments starting with '.' (other than the
 *     two above, which are always rejected as traversal).
 *
 * Returns OK / FORBIDDEN / BAD_REQUEST. */
static http_static_path_result_t
validate_segments(const http_static_handler_t *mount,
                  const char *path, size_t path_len)
{
    /* Skip the leading '/' that path always starts with. */
    size_t i = 0;
    if (path_len > 0 && path[0] == '/') {
        i = 1;
    }

    while (i < path_len) {
        size_t seg_start = i;
        while (i < path_len && path[i] != '/') {
            i++;
        }
        const size_t seg_len = i - seg_start;
        const char *seg = path + seg_start;

        if (seg_len == 0) {
            /* Empty segment from "//" — reject. */
            return HTTP_STATIC_PATH_BAD_REQUEST;
        }
        if (seg_len == 1 && seg[0] == '.') {
            return HTTP_STATIC_PATH_FORBIDDEN;
        }
        if (seg_len == 2 && seg[0] == '.' && seg[1] == '.') {
            return HTTP_STATIC_PATH_FORBIDDEN;
        }
        if (seg[0] == '.') {
            /* Genuine dotfile (.git, .htaccess, ...). Apply policy. */
            if (mount->flags & HTTP_STATIC_FLAG_DOTFILES_DENY) {
                return HTTP_STATIC_PATH_FORBIDDEN;
            }
            if (mount->flags & HTTP_STATIC_FLAG_DOTFILES_IGNORE) {
                /* Caller treats "hidden" same as on_missing. */
                return HTTP_STATIC_PATH_HIDE;
            }
            /* HTTP_STATIC_FLAG_DOTFILES_ALLOW: fall through. */
        }
        /* Skip the '/' that terminated the segment, if any. */
        if (i < path_len) {
            i++;
        }
    }

    return HTTP_STATIC_PATH_OK;
}

http_static_path_result_t
http_static_path_resolve(const http_static_handler_t *mount,
                         const char *request_path, size_t request_path_len,
                         char *out_buf, size_t out_buf_cap, size_t *out_len,
                         const char **out_relative, size_t *out_relative_len)
{
    if (UNEXPECTED(mount == NULL || mount->url_prefix == NULL ||
                   mount->root_directory == NULL)) {
        return HTTP_STATIC_PATH_NO_MATCH;
    }
    if (request_path_len == 0 || request_path[0] != '/') {
        /* Absolute-form / authority-form / asterisk targets are rejected
         * by upstream parsers before reaching here; defensive. */
        return HTTP_STATIC_PATH_BAD_REQUEST;
    }

    /* Prefix match: request must START with the mount's url_prefix.
     * Both strings are guaranteed to start and end with '/'. */
    const size_t prefix_len = mount->url_prefix_len;
    if (request_path_len < prefix_len) {
        return HTTP_STATIC_PATH_NO_MATCH;
    }
    if (memcmp(request_path, ZSTR_VAL(mount->url_prefix), prefix_len) != 0) {
        return HTTP_STATIC_PATH_NO_MATCH;
    }

    /* Tail starts at the prefix's terminating '/' so segments share
     * the exact same separator semantics as the rest of the path —
     * "/static/foo.css" → tail "foo.css", with the join below
     * inserting '/' against root. */
    const char *tail     = request_path + prefix_len;
    size_t      tail_len = request_path_len - prefix_len;

    /* Strip URL query string / fragment if the upstream parser left
     * them attached — defensive; llhttp normally hands us the path
     * component only, but the H2/H3 path may differ. */
    for (size_t i = 0; i < tail_len; i++) {
        if (tail[i] == '?' || tail[i] == '#') {
            tail_len = i;
            break;
        }
    }

    /* Decode into a scratch buffer first; we then concatenate root +
     * '/' + decoded into out_buf. */
    char decoded[PATH_MAX];
    size_t decoded_len = 0;
    const http_static_path_result_t decode_rc =
        percent_decode(tail, tail_len, decoded, sizeof(decoded), &decoded_len);
    if (decode_rc != HTTP_STATIC_PATH_OK) {
        return decode_rc;
    }

    /* validate_segments() expects a leading '/' — synthesize one. */
    char prefixed[PATH_MAX];
    if (decoded_len + 1 >= sizeof(prefixed)) {
        return HTTP_STATIC_PATH_BAD_REQUEST;
    }
    prefixed[0] = '/';
    memcpy(prefixed + 1, decoded, decoded_len);
    prefixed[decoded_len + 1] = '\0';

    /* Empty tail (URL == prefix exactly) is OK — the FSM will try the
     * index files. validate_segments would reject the empty-segment
     * case otherwise, so short-circuit. */
    if (decoded_len == 0) {
        /* fall through to concatenation below — the resulting path is
         * just the root directory. */
    } else {
        const http_static_path_result_t seg_rc =
            validate_segments(mount, prefixed, decoded_len + 1);
        if (seg_rc != HTTP_STATIC_PATH_OK) {
            return seg_rc;
        }
    }

    /* Concatenate root + '/' + decoded into out_buf. Root is already
     * canonical (no trailing '/' from realpath, except when root is
     * exactly "/"). */
    const char *const root     = ZSTR_VAL(mount->root_directory);
    const size_t      root_len = ZSTR_LEN(mount->root_directory);

    /* Need root_len + 1 (slash) + decoded_len + 1 (NUL). */
    if (root_len + 1 + decoded_len + 1 > out_buf_cap) {
        return HTTP_STATIC_PATH_BAD_REQUEST;
    }
    memcpy(out_buf, root, root_len);
    size_t out = root_len;
    /* Avoid a duplicate '/' when root already ends with one (root="/"). */
    if (out > 0 && out_buf[out - 1] != '/') {
        out_buf[out++] = '/';
    }
    memcpy(out_buf + out, decoded, decoded_len);
    out_buf[out + decoded_len] = '\0';
    *out_len = out + decoded_len;

    if (out_relative != NULL) {
        *out_relative     = out_buf + out;
        *out_relative_len = decoded_len;
    }

    return HTTP_STATIC_PATH_OK;
}

bool http_static_path_join(char *buf, size_t cap, size_t *len,
                           const char *name, size_t name_len)
{
    size_t cur = *len;
    /* Need '/' + name + NUL. Skip the slash if buf already ends with
     * one (e.g. when buf is the canonical root "/"). */
    const bool need_sep = (cur == 0 || buf[cur - 1] != '/');
    const size_t extra  = (need_sep ? 1 : 0) + name_len + 1;
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

bool http_static_path_is_hidden(const http_static_handler_t *mount,
                                const char *relative, size_t relative_len)
{
    if (mount == NULL || mount->hide_count == 0) {
        return false;
    }
    /* fnmatch wants a NUL-terminated string; the caller may pass a
     * stretch of a larger buffer, so copy into a scratch. */
    if (relative_len >= PATH_MAX) {
        return false;
    }
    char scratch[PATH_MAX];
    memcpy(scratch, relative, relative_len);
    scratch[relative_len] = '\0';

    for (size_t i = 0; i < mount->hide_count; i++) {
        const zend_string *glob = mount->hide_globs[i];
        if (fnmatch(ZSTR_VAL(glob), scratch, FNM_PATHNAME) == 0) {
            return true;
        }
    }
    return false;
}
