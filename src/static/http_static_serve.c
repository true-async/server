/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

/* Static-handler dispatch (issue #13). Resolves the request URL
 * against the configured mounts, opens the file synchronously, and
 * populates the response object so the existing handler dispose path
 * flushes it on the wire. The PHP coroutine entry sees
 * ctx->skip_php_handler and short-circuits without entering the VM —
 * no zend_call_function, no zend_try, no fcall_info_cache lookup.
 *
 * The synchronous open/read is intentional for the first cut. PR #5 in
 * docs/PLAN_STATIC_HANDLER.md upgrades the read to a libuv thread-pool
 * async chain (or sendfile/splice). The interface here — single
 * http_static_try_serve entrypoint — is stable across that change. */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include "php.h"
#include "zend_smart_str.h"
#include "php_http_server.h"
#include "core/http_connection.h"
#include "core/http_connection_internal.h"
#include "static/static_handler.h"
#include "static/http_static_mime.h"
#include "static/http_static_path.h"
#include "static/http_static_etag.h"

#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <string.h>

/* Helper: emit a canned status-only response on the static path. The
 * response object is fully PHP-owned but not yet committed; we set
 * status + (optional) message body and let dispose flush. */
static void emit_status(zend_object *response_obj, int status,
                        const char *body_msg, size_t body_msg_len)
{
    http_response_static_set_status(response_obj, status);
    http_response_static_set_header(response_obj,
        "content-type", 12, "text/plain; charset=utf-8", 25);
    if (body_msg != NULL && body_msg_len > 0) {
        zend_string *msg = zend_string_init(body_msg, body_msg_len, 0);
        http_response_static_set_body_str(response_obj, msg);
        zend_string_release(msg);
    }
}

/* Locate the request method (GET/HEAD/...) on the parsed request. */
static inline bool method_is_get(const http_request_t *req)
{
    return req != NULL && req->method != NULL
        && ZSTR_LEN(req->method) == 3
        && memcmp(ZSTR_VAL(req->method), "GET", 3) == 0;
}

static inline bool method_is_head(const http_request_t *req)
{
    return req != NULL && req->method != NULL
        && ZSTR_LEN(req->method) == 4
        && memcmp(ZSTR_VAL(req->method), "HEAD", 4) == 0;
}

/* Lookup a request header by lower-case name. Returns NULL on miss. */
static const zend_string *find_request_header(const http_request_t *req,
                                              const char *name, size_t name_len)
{
    if (req == NULL || req->headers == NULL) {
        return NULL;
    }
    zval *zv = zend_hash_str_find(req->headers, name, name_len);
    if (zv == NULL) {
        return NULL;
    }
    if (Z_TYPE_P(zv) == IS_STRING) {
        return Z_STR_P(zv);
    }
    if (Z_TYPE_P(zv) == IS_ARRAY) {
        zval *first = zend_hash_index_find(Z_ARRVAL_P(zv), 0);
        if (first != NULL && Z_TYPE_P(first) == IS_STRING) {
            return Z_STR_P(first);
        }
    }
    return NULL;
}

/* Open with the right flags for the configured symlink policy. Returns
 * a non-negative fd on success, -1 on failure (errno set). */
static int open_for_policy(const http_static_handler_t *mount, const char *path)
{
    int flags = O_RDONLY | O_CLOEXEC;
#ifdef O_NOFOLLOW
    if (mount->flags & HTTP_STATIC_FLAG_SYMLINKS_REJECT) {
        flags |= O_NOFOLLOW;
    }
#endif
    return open(path, flags);
}

/* Slurp `size` bytes from `fd` into a freshly allocated zend_string.
 * Returns the zend_string on success (caller releases), NULL on
 * partial-read / I/O error. */
static zend_string *slurp_fd(int fd, size_t size)
{
    if (size == 0) {
        return ZSTR_EMPTY_ALLOC();
    }
    zend_string *out = zend_string_alloc(size, 0);
    size_t total = 0;
    while (total < size) {
        const ssize_t n = read(fd, ZSTR_VAL(out) + total, size - total);
        if (n > 0) {
            total += (size_t)n;
            continue;
        }
        if (n == 0) break;                /* premature EOF */
        if (errno == EINTR) continue;
        zend_string_release(out);
        return NULL;
    }
    if (total != size) {
        zend_string_release(out);
        return NULL;
    }
    ZSTR_VAL(out)[size] = '\0';
    return out;
}

/* Apply mount-configured extra headers (set via setHeader()) to the
 * response. Skips Content-* members on a 304 emission per RFC 9110
 * §15.4.5; pass include_content_headers=true on 200 / false on 304. */
static void apply_extra_headers(zend_object *response_obj,
                                const http_static_handler_t *mount,
                                bool include_content_headers)
{
    if (mount->extra_headers == NULL) {
        return;
    }
    zend_string *name;
    zval        *value;
    ZEND_HASH_FOREACH_STR_KEY_VAL(mount->extra_headers, name, value) {
        if (name == NULL || Z_TYPE_P(value) != IS_STRING) {
            continue;
        }
        if (!include_content_headers
            && ZSTR_LEN(name) >= 8
            && strncasecmp(ZSTR_VAL(name), "content-", 8) == 0) {
            continue;
        }
        http_response_static_set_header(response_obj,
            ZSTR_VAL(name), ZSTR_LEN(name),
            Z_STRVAL_P(value), Z_STRLEN_P(value));
    } ZEND_HASH_FOREACH_END();
}

/* Try one candidate file path: open + fstat. Returns:
 *   true → fd opened, *st filled, *out_fd is the fd (caller owns close)
 *   false → not openable (errno set; ENOENT means try the next index
 *           candidate, anything else is a hard failure)
 *
 * Symlink policy is enforced via O_NOFOLLOW; the rejected case surfaces
 * as ELOOP from open(). */
static bool try_open_candidate(const http_static_handler_t *mount,
                               const char *path,
                               int *out_fd, struct stat *st)
{
    const int fd = open_for_policy(mount, path);
    if (fd < 0) {
        return false;
    }
    if (fstat(fd, st) != 0) {
        const int saved_errno = errno;
        close(fd);
        errno = saved_errno;
        return false;
    }
    if (!S_ISREG(st->st_mode)) {
        /* Directory or special file — the caller decides whether this
         * is "try the index" or "404". Surface as ENOENT so the
         * fallthrough mirrors a missing file. */
        close(fd);
        errno = ENOENT;
        return false;
    }
    *out_fd = fd;
    return true;
}

/* Decide: was the resolved URL path a directory (ends with '/' or
 * empty)? Directory requests trigger the index-file lookup. */
static inline bool path_targets_directory(const char *relative, size_t relative_len)
{
    return relative_len == 0 || relative[relative_len - 1] == '/';
}

http_static_result_t http_static_try_serve(http_server_object *server,
                                           http_connection_t *conn,
                                           void *ctx_void,
                                           http_request_t *request)
{
    const size_t mount_count = http_static_handler_count(server);
    if (UNEXPECTED(mount_count == 0)) {
        return HTTP_STATIC_PASSTHROUGH;
    }
    (void)conn;  /* will be used by the future async serve path. */

    http1_request_ctx_t *const ctx = (http1_request_ctx_t *)ctx_void;
    zend_object *const response_obj = Z_OBJ(ctx->response_zv);

    /* Static handler covers GET + HEAD only. Other methods fall through
     * to the regular PHP handler — operators may overlay POST endpoints
     * on the same prefix without a 405 surprise. */
    const bool is_head = method_is_head(request);
    const bool is_get  = method_is_get(request);
    if (!is_get && !is_head) {
        return HTTP_STATIC_PASSTHROUGH;
    }

    /* Use request->uri (always populated by the parser) — request->path
     * is lazy-built only on the PHP-side getter. http_static_path_resolve
     * strips '?' and '#' itself, so feeding the whole URI is fine. */
    const char  *req_path     = (request->uri != NULL) ? ZSTR_VAL(request->uri) : NULL;
    const size_t req_path_len = (request->uri != NULL) ? ZSTR_LEN(request->uri) : 0;
    if (UNEXPECTED(req_path == NULL || req_path_len == 0)) {
        return HTTP_STATIC_PASSTHROUGH;
    }

    for (size_t mi = 0; mi < mount_count; mi++) {
        const http_static_handler_t *const mount =
            http_static_handler_get(server, mi);
        if (UNEXPECTED(mount == NULL)) continue;

        char fs_path[PATH_MAX];
        size_t fs_path_len = 0;
        const char *relative = NULL;
        size_t relative_len = 0;

        const http_static_path_result_t rc = http_static_path_resolve(
            mount, req_path, req_path_len,
            fs_path, sizeof(fs_path), &fs_path_len,
            &relative, &relative_len);

        if (rc == HTTP_STATIC_PATH_NO_MATCH) {
            continue;  /* try next mount */
        }
        if (rc == HTTP_STATIC_PATH_BAD_REQUEST) {
            emit_status(response_obj, 400, "Bad Request", 11);
            ctx->skip_php_handler = true;
            return HTTP_STATIC_HANDLED;
        }
        if (rc == HTTP_STATIC_PATH_FORBIDDEN) {
            /* Dotfile deny / traversal escape: 404 by convention so we
             * don't disclose the existence of restricted resources. */
            emit_status(response_obj, 404, "Not Found", 9);
            ctx->skip_php_handler = true;
            return HTTP_STATIC_HANDLED;
        }
        if (rc == HTTP_STATIC_PATH_HIDE) {
            /* Dotfile-ignore mode: behave like a missing file so the
             * on_missing policy applies. */
            if (mount->flags & HTTP_STATIC_FLAG_ON_MISSING_NEXT) {
                return HTTP_STATIC_PASSTHROUGH;
            }
            emit_status(response_obj, 404, "Not Found", 9);
            ctx->skip_php_handler = true;
            return HTTP_STATIC_HANDLED;
        }

        /* Hide-globs are matched against the path RELATIVE to root —
         * not the full filesystem path — so the pattern syntax matches
         * what operators see when configuring. */
        if (relative_len > 0
            && http_static_path_is_hidden(mount, relative, relative_len)) {
            if (mount->flags & HTTP_STATIC_FLAG_ON_MISSING_NEXT) {
                return HTTP_STATIC_PASSTHROUGH;
            }
            emit_status(response_obj, 404, "Not Found", 9);
            ctx->skip_php_handler = true;
            return HTTP_STATIC_HANDLED;
        }

        int fd = -1;
        struct stat st;
        bool opened = false;

        if (path_targets_directory(relative, relative_len)) {
            /* Directory request → walk the configured index files. The
             * loop allocates no temporaries: each candidate path is
             * built in-place into fs_path and rewound on miss. */
            for (size_t ii = 0; ii < mount->index_count; ii++) {
                const zend_string *idx = mount->index_files[ii];
                size_t cand_len = fs_path_len;
                if (!http_static_path_join(fs_path, sizeof(fs_path),
                                           &cand_len,
                                           ZSTR_VAL(idx), ZSTR_LEN(idx))) {
                    continue;
                }
                if (try_open_candidate(mount, fs_path, &fd, &st)) {
                    opened = true;
                    /* Promote the candidate length so MIME lookup sees
                     * the index file's extension, not the directory's. */
                    fs_path_len = cand_len;
                    break;
                }
                /* Rewind by truncating to the directory prefix. */
                fs_path[fs_path_len] = '\0';
            }
        } else {
            opened = try_open_candidate(mount, fs_path, &fd, &st);
        }

        if (!opened) {
            const int e = errno;
            if (e == EACCES || e == EPERM) {
                emit_status(response_obj, 403, "Forbidden", 9);
                ctx->skip_php_handler = true;
                return HTTP_STATIC_HANDLED;
            }
            /* ENOENT / ELOOP (symlink rejected) / ENOTDIR / others:
             * treat as "no match" per on_missing policy. */
            if (mount->flags & HTTP_STATIC_FLAG_ON_MISSING_NEXT) {
                return HTTP_STATIC_PASSTHROUGH;
            }
            emit_status(response_obj, 404, "Not Found", 9);
            ctx->skip_php_handler = true;
            return HTTP_STATIC_HANDLED;
        }

        /* Cap synchronous reads to avoid blocking the event loop on a
         * stray giant file. The MAX is plenty for static-asset
         * deployments and a future async path will remove the cap. */
        if ((uint64_t)st.st_size > (uint64_t)HTTP_STATIC_MAX_FILE_SIZE) {
            close(fd);
            emit_status(response_obj, 413, "Payload Too Large", 17);
            ctx->skip_php_handler = true;
            return HTTP_STATIC_HANDLED;
        }

        /* ETag + Last-Modified — computed once, used for both the 304
         * decision and the 200 response. */
        char etag_buf[HTTP_STATIC_ETAG_BUF_LEN];
        const bool etag_enabled = (mount->flags & HTTP_STATIC_FLAG_ETAG) != 0;
        if (etag_enabled) {
            http_static_etag_format(&st, etag_buf);
        }

        char last_modified_buf[HTTP_STATIC_DATE_BUF_LEN];
        http_static_format_http_date(st.st_mtime, last_modified_buf);

        /* Conditional GET. */
        const zend_string *if_none_match = find_request_header(request,
            "if-none-match", 13);
        const zend_string *if_modified_since = find_request_header(request,
            "if-modified-since", 17);
        const bool not_modified = http_static_conditional_match(
            if_none_match    != NULL ? ZSTR_VAL(if_none_match)    : NULL,
            if_none_match    != NULL ? ZSTR_LEN(if_none_match)    : 0,
            if_modified_since != NULL ? ZSTR_VAL(if_modified_since) : NULL,
            if_modified_since != NULL ? ZSTR_LEN(if_modified_since) : 0,
            etag_enabled ? etag_buf : NULL,
            etag_enabled ? strlen(etag_buf) : 0,
            st.st_mtime);

        if (not_modified) {
            close(fd);
            http_response_static_set_status(response_obj, 304);
            if (etag_enabled) {
                http_response_static_set_header(response_obj,
                    "etag", 4, etag_buf, strlen(etag_buf));
            }
            http_response_static_set_header(response_obj,
                "last-modified", 13,
                last_modified_buf, strlen(last_modified_buf));
            if (mount->cache_control != NULL) {
                http_response_static_set_header(response_obj,
                    "cache-control", 13,
                    ZSTR_VAL(mount->cache_control),
                    ZSTR_LEN(mount->cache_control));
            }
            apply_extra_headers(response_obj, mount,
                                /* include_content_headers */ false);
            ctx->skip_php_handler = true;
            return HTTP_STATIC_HANDLED;
        }

        /* 200 path: read body, populate response. HEAD reuses the same
         * headers but emits an empty body — the dispose formatter
         * preserves Content-Length from the body length, so for HEAD
         * we still need to advertise the size. Fix that by setting
         * content-length explicitly when body is empty. */
        zend_string *body = NULL;
        if (is_get) {
            body = slurp_fd(fd, (size_t)st.st_size);
            if (body == NULL) {
                close(fd);
                emit_status(response_obj, 500, "Internal Server Error", 21);
                ctx->skip_php_handler = true;
                return HTTP_STATIC_HANDLED;
            }
        }
        close(fd);

        /* Build headers. */
        http_response_static_set_status(response_obj, 200);

        const char *content_type = NULL;
        size_t      content_type_len = 0;
        if (!http_static_mime_lookup(mount, fs_path, fs_path_len,
                                     &content_type, &content_type_len)) {
            content_type     = "application/octet-stream";
            content_type_len = strlen(content_type);
        }
        http_response_static_set_header(response_obj,
            "content-type", 12, content_type, content_type_len);

        if (etag_enabled) {
            http_response_static_set_header(response_obj,
                "etag", 4, etag_buf, strlen(etag_buf));
        }
        http_response_static_set_header(response_obj,
            "last-modified", 13,
            last_modified_buf, strlen(last_modified_buf));
        if (mount->cache_control != NULL) {
            http_response_static_set_header(response_obj,
                "cache-control", 13,
                ZSTR_VAL(mount->cache_control),
                ZSTR_LEN(mount->cache_control));
        }
        apply_extra_headers(response_obj, mount,
                            /* include_content_headers */ true);

        if (is_head) {
            /* HEAD: advertise Content-Length explicitly so it lines up
             * with the equivalent GET. The format-time path computes
             * Content-Length from the body smart_str otherwise. */
            char clen[32];
            const int n = snprintf(clen, sizeof(clen), "%" PRIu64,
                                   (uint64_t)st.st_size);
            if (n > 0 && (size_t)n < sizeof(clen)) {
                http_response_static_set_header(response_obj,
                    "content-length", 14, clen, (size_t)n);
            }
        } else {
            http_response_static_set_body_str(response_obj, body);
            zend_string_release(body);
        }

        ctx->skip_php_handler = true;
        return HTTP_STATIC_HANDLED;
    }

    return HTTP_STATIC_PASSTHROUGH;
}
