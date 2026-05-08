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
#include "Zend/zend_async_API.h"
#include "php_http_server.h"
#include "core/http_connection.h"
#include "core/http_connection_internal.h"
#ifdef HAVE_OPENSSL
# include "core/tls_layer.h"
#endif
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
#ifdef __linux__
# include <sys/socket.h>
# include <netinet/in.h>
# include <netinet/tcp.h>   /* TCP_CORK */
#endif

static void emit_status(zend_object *response_obj, int status,
                        const char *body_msg, size_t body_msg_len)
{
    http_response_static_set_status(response_obj, status);
    http_response_static_set_header(response_obj,
        "content-type", 12, "text/plain; charset=utf-8", 25);
    if (body_msg != NULL && body_msg_len > 0) {
        zend_string *const msg = zend_string_init(body_msg, body_msg_len, 0);
        http_response_static_set_body_str(response_obj, msg);
        zend_string_release(msg);
    }
}

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

static const zend_string *find_request_header(const http_request_t *req,
                                              const char *name, size_t name_len)
{
    if (req == NULL || req->headers == NULL) {
        return NULL;
    }
    const zval *const zv = zend_hash_str_find(req->headers, name, name_len);
    if (zv == NULL) {
        return NULL;
    }
    if (Z_TYPE_P(zv) == IS_STRING) {
        return Z_STR_P(zv);
    }
    if (Z_TYPE_P(zv) == IS_ARRAY) {
        const zval *const first = zend_hash_index_find(Z_ARRVAL_P(zv), 0);
        if (first != NULL && Z_TYPE_P(first) == IS_STRING) {
            return Z_STR_P(first);
        }
    }
    return NULL;
}

static int open_for_policy(const http_static_handler_t *mount, const char *path)
{
    int flags = O_RDONLY | O_CLOEXEC;
#ifdef O_NOFOLLOW
    /* OWNER mode is documented as "follow if owner-of-link == owner-
     * of-target" but the post-open uid comparison isn't implemented
     * yet — alias to REJECT so the policy is no weaker than the
     * advertised security promise. The real owner-match check lands
     * with the async serve path. */
    if (mount->flags & (HTTP_STATIC_FLAG_SYMLINKS_REJECT
                      | HTTP_STATIC_FLAG_SYMLINKS_OWNER)) {
        flags |= O_NOFOLLOW;
    }
#endif
    return open(path, flags);
}

/* After try_open_candidate succeeds we know the FINAL component is
 * not a symlink (O_NOFOLLOW handles that). Intermediate components
 * are still followed by open(2), so a symlink at any level inside
 * the mount root could redirect us outside. realpath()-based prefix
 * verification closes that gap. The TOCTOU between realpath() and
 * the open we already did is acceptable — exploiting it requires
 * filesystem write access on the host. */
static bool resolved_under_root(const http_static_handler_t *mount,
                                const char *path)
{
    char canonical[PATH_MAX];
    if (UNEXPECTED(realpath(path, canonical) == NULL)) {
        return false;
    }
    const char *const root     = ZSTR_VAL(mount->root_directory);
    const size_t      root_len = ZSTR_LEN(mount->root_directory);

    if (strncmp(canonical, root, root_len) != 0) {
        return false;
    }
    /* canonical == root exactly, or canonical[root_len] is a separator
     * (subpath). Otherwise canonical only happens to share a prefix
     * (e.g. root="/srv/foo", canonical="/srv/foobar/x"). */
    const char tail = canonical[root_len];
    return tail == '\0' || tail == '/';
}

static zend_string *slurp_fd(const int fd, const size_t size)
{
    if (size == 0) {
        return ZSTR_EMPTY_ALLOC();
    }
    zend_string *const out = zend_string_alloc(size, 0);
    size_t total = 0;
    while (total < size) {
        const ssize_t n = read(fd, ZSTR_VAL(out) + total, size - total);
        if (EXPECTED(n > 0)) {
            total += (size_t)n;
            continue;
        }
        if (n == 0) break;                /* premature EOF */
        if (errno == EINTR) continue;
        zend_string_release(out);
        return NULL;
    }
    if (UNEXPECTED(total != size)) {
        zend_string_release(out);
        return NULL;
    }
    ZSTR_VAL(out)[size] = '\0';
    return out;
}

/* RFC 9110 §15.4.5: 304 must NOT carry Content-* headers — pass
 * include_content_headers=false on the not-modified path. */
static void apply_extra_headers(zend_object *response_obj,
                                const http_static_handler_t *mount,
                                const bool include_content_headers)
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

/* Try open + fstat. On a non-regular file, surface ENOENT so the
 * caller's fallthrough mirrors the missing-file path uniformly. */
static bool try_open_candidate(const http_static_handler_t *mount,
                               const char *path,
                               int *out_fd, struct stat *st)
{
    const int fd = open_for_policy(mount, path);
    if (fd < 0) {
        return false;
    }
    if (UNEXPECTED(fstat(fd, st) != 0)) {
        const int saved_errno = errno;
        close(fd);
        errno = saved_errno;
        return false;
    }
    if (UNEXPECTED(!S_ISREG(st->st_mode))) {
        close(fd);
        errno = ENOENT;
        return false;
    }
    *out_fd = fd;
    return true;
}

static inline bool path_targets_directory(const char *relative,
                                          const size_t relative_len)
{
    return relative_len == 0 || relative[relative_len - 1] == '/';
}

/* ===== Hard-zero async path =========================================
 *
 * Plain-TCP and kTLS-engaged connections take this path: the request
 * lifetime is owned by a callback chain rooted at ZEND_ASYNC_FS_OPEN,
 * never spawns a coroutine, never enters the PHP VM. Each phase
 * (open → stat → headers → sendfile → close → finalize) lives in its
 * own callback subscribed to either the file io's event or a
 * sendfile req's event. http_request_finalize closes out the chain
 * the same way the regular coroutine dispose does, so keep-alive,
 * drain, and pipelined-request resume all keep working.
 *
 * User-space TLS connections fall back to the synchronous-populate
 * path below — sendfile would bypass OpenSSL, and re-routing the
 * read+write loop through http_connection_send buys us nothing the
 * existing code path doesn't already do. */

/* Single persistent callback model: one callback registered once on
 * file_io->event for the lifetime of the chain. Phase advances on
 * each completion. Spurious fires (a callback registered mid-NOTIFY
 * can re-enter the same NOTIFY iteration) are filtered by phase
 * versus expected-req identity, mirroring http_log's writer_cb. */
typedef enum {
    SS_PHASE_OPEN     = 0,  /* awaiting fs_open completion (result=NULL) */
    SS_PHASE_STAT     = 1,  /* awaiting io_stat completion (result=stat req) */
    SS_PHASE_SENDFILE = 2,  /* awaiting sendfile completion (result=sendfile req) */
    SS_PHASE_DONE     = 3,
} ss_phase_t;

typedef struct {
    http_connection_t   *conn;
    http1_request_ctx_t *ctx;
    const http_static_handler_t *mount;

    /* Resolved on-disk path. emalloc'd in ss_kick_off, freed by
     * ss_state_free.  Used to be a 4 KiB inline scratch — with 10 K
     * concurrent in-flight static requests that would have been 40 MiB
     * just for path strings, most of which fit comfortably in <100
     * bytes (#13 in TODO_STATIC_HANDLER_REVIEW). */
    char                *fs_path;
    size_t               fs_path_len;

    /* Async file io. Acquired by ZEND_ASYNC_FS_OPEN, disposed at the
     * end of the chain. Pending until SS_PHASE_OPEN fires. */
    zend_async_io_t     *file_io;

    /* Cached fstat. */
    struct stat          st;

    bool                 is_head;
    bool                 should_continue;  /* keep-alive verdict */

    /* State machine cursor. */
    ss_phase_t           phase;

    /* Identity of the currently-pending op's req. NOTIFY may fire our
     * cb spuriously (registration during NOTIFY iteration races) —
     * we ignore any result that doesn't match. NULL during the OPEN
     * phase because libuv_fs_open notifies with result=NULL. */
    zend_async_io_req_t *pending_req;

    /* Persistent cb registered once on file_io->event at kick-off,
     * removed once at finalize. */
    zend_async_event_callback_t *cb;
} ss_state_t;

typedef struct {
    zend_async_event_callback_t base;
    ss_state_t *state;
} ss_cb_t;

static void ss_dispatch(zend_async_event_t *event,
                        zend_async_event_callback_t *callback,
                        void *result, zend_object *exception);

static void ss_cb_dispose(zend_async_event_callback_t *cb,
                          zend_async_event_t *event)
{
    (void)event;
    efree(cb);
}

/* Single owner of state lifetime. Always frees fs_path (NULL-safe via
 * efree) before efree'ing the state struct. */
static inline void ss_state_free(ss_state_t *state)
{
    if (state == NULL) return;
    if (state->fs_path != NULL) {
        efree(state->fs_path);
        state->fs_path = NULL;
    }
    efree(state);
}

/* TCP_CORK gate (Linux). Headers are submitted fire-and-forget through
 * the existing batched uv_write queue; sendfile then writes directly to
 * the same fd via uv_fs_sendfile. Without serialisation, on a slow /
 * congested socket the two could interleave on the wire. Corking the
 * socket from kick-off through finalize forces the kernel to coalesce
 * (and to NEVER reorder partial writes against sendfile output) at the
 * cost of one extra setsockopt round trip per response.
 *
 * Plain-TCP only — the hard-zero path is already plain-TCP gated via
 * conn_supports_sendfile, so we don't need to inspect TLS state here. */
static inline void ss_cork_set(http_connection_t *conn, const int on)
{
#ifdef TCP_CORK
    if (UNEXPECTED(conn == NULL || conn->io == NULL)) {
        return;
    }
    if (conn->io->type != ZEND_ASYNC_IO_TYPE_TCP) {
        return;
    }
    const int fd = (int) conn->io->descriptor.socket;
    if (UNEXPECTED(fd < 0)) {
        return;
    }
    /* setsockopt failure here is non-fatal: worst case we skip the
     * coalescing optimisation. Sendfile + headers still go out. */
    (void) setsockopt(fd, IPPROTO_TCP, TCP_CORK, &on, sizeof(on));
#else
    (void) conn; (void) on;
#endif
}

/* Tear down the state machine and run http_request_finalize. After
 * this the conn is either keep-alive-armed for the next request or
 * closed; either way nothing more touches `state`. */
static void ss_finalize(ss_state_t *state)
{
    http_connection_t *const conn = state->conn;
    http1_request_ctx_t *const ctx = state->ctx;

    state->phase = SS_PHASE_DONE;

    /* Uncork before tearing down so the kernel flushes whatever's left
     * (typically the trailing chunk of the sendfile body). Issued
     * unconditionally — TCP_CORK off is a no-op on a non-corked socket
     * and a no-op on non-Linux. Pairs with the cork in ss_kick_off. */
    ss_cork_set(conn, 0);

    if (state->cb != NULL && state->file_io != NULL) {
        (void) state->file_io->event.del_callback(
            &state->file_io->event, state->cb);
        state->cb = NULL;
    }
    if (state->file_io != NULL) {
        if (state->file_io->event.dispose != NULL) {
            state->file_io->event.dispose(&state->file_io->event);
        }
        state->file_io = NULL;
    }

    /* Pair the on_request_dispatch fired at hand-off. */
    http_server_on_request_dispose(conn->counters);

    /* Mirror the dispose-side bookkeeping that the coroutine path
     * normally handles for us. */
    if (ctx->request != NULL) {
        ctx->request->coroutine = NULL;
    }
    if (conn->current_request == ctx->request) {
        conn->current_request = NULL;
    }

    const bool should_continue = state->should_continue;
    ss_state_free(state);

    http_request_finalize(conn, ctx, should_continue);
}

/* Borrow the pre-rendered HTTP/1.1 status line from the shared table
 * in http_response.c (single source of truth — see #10 in
 * TODO_STATIC_HANDLER_REVIEW). The static handler only ever emits a
 * narrow subset of codes (200/304/4xx/413/500) so falling back to
 * 500 on an unknown code preserves the previous behaviour. */
static const char *ss_status_line(const int status, size_t *out_len)
{
    const char *line = http_response_status_line_http11(status, out_len);
    if (UNEXPECTED(line == NULL)) {
        line = http_response_status_line_http11(500, out_len);
    }
    return line;
}

/* Build the full response head (status + headers + optional inline
 * body) into a fresh zend_string and submit it fire-and-forget via
 * the existing fire-and-forget send. We MUST NOT suspend in this
 * callback chain — there is no coroutine to park — so the await-
 * style http_connection_send_raw is off-limits. The reactor takes
 * ownership of the string and releases on write completion. */
static bool ss_send_response(ss_state_t *state, const int status_code,
                             const smart_str *headers,
                             const char *body, const size_t body_len)
{
    smart_str line = {0};
    size_t status_line_len = 0;
    const char *const status_line = ss_status_line(status_code, &status_line_len);
    smart_str_appendl(&line, status_line, status_line_len);
    if (headers != NULL && headers->s != NULL) {
        smart_str_append_smart_str(&line, headers);
    }
    smart_str_appends(&line, "\r\n");
    if (body != NULL && body_len > 0) {
        smart_str_appendl(&line, body, body_len);
    }
    smart_str_0(&line);

    /* Transfer ownership of line.s to the reactor. */
    zend_string *const owned = line.s;
    line.s = NULL;
    return http_connection_send_str_owned(state->conn, owned);
}

/* Append one header line to `out`. Avoids string-init churn. */
static void ss_append_header(smart_str *out,
                             const char *name, size_t name_len,
                             const char *value, size_t value_len)
{
    smart_str_appendl(out, name, name_len);
    smart_str_appends(out, ": ");
    smart_str_appendl(out, value, value_len);
    smart_str_appends(out, "\r\n");
}

/* Build the response headers block (without the leading status line
 * and without the trailing CRLF — ss_send_response adds those).
 * include_content_headers gates Content-* on the 304 path
 * (RFC 9110 §15.4.5). */
static void ss_build_headers(ss_state_t *state, smart_str *out,
                             const char *content_type, size_t content_type_len,
                             const char *etag_buf, bool etag_enabled,
                             const char *last_modified_buf,
                             const uint64_t content_length,
                             const bool include_content_headers)
{
    if (include_content_headers && content_type != NULL) {
        ss_append_header(out, "Content-Type", 12,
                         content_type, content_type_len);
    }
    if (include_content_headers) {
        char clen[32];
        const int n = snprintf(clen, sizeof(clen), "%" PRIu64, content_length);
        if (n > 0 && (size_t)n < sizeof(clen)) {
            ss_append_header(out, "Content-Length", 14, clen, (size_t)n);
        }
    }
    if (etag_enabled) {
        ss_append_header(out, "ETag", 4, etag_buf, HTTP_STATIC_ETAG_LEN);
    }
    ss_append_header(out, "Last-Modified", 13,
                     last_modified_buf, HTTP_STATIC_DATE_LEN);

    /* Cache-Control + extra_headers are pre-rendered into one persistent
     * string at freeze-time (#6 in TODO_STATIC_HANDLER_REVIEW). Splice
     * the right variant in with a single append; falling back to the
     * iterator path is only needed if freeze somehow ran with prebakes
     * NULL (shouldn't happen post-lock, but stays correct if it does). */
    zend_string *const prebaked = include_content_headers
        ? state->mount->prebaked_headers_full
        : state->mount->prebaked_headers_no_content;
    if (prebaked != NULL) {
        smart_str_append(out, prebaked);
    }
    if (state->conn->keep_alive) {
        ss_append_header(out, "Connection", 10, "keep-alive", 10);
    } else {
        ss_append_header(out, "Connection", 10, "close", 5);
    }
}

/* Synchronous error emission shortcut. Used when something fails
 * mid-chain (open ENOENT, stat error, sendfile error). */
static void ss_emit_error(ss_state_t *state, const int status_code,
                          const char *body)
{
    smart_str h = {0};
    ss_append_header(&h, "Content-Type", 12, "text/plain; charset=utf-8", 25);
    char clen[32];
    const size_t body_len = body != NULL ? strlen(body) : 0;
    const int n = snprintf(clen, sizeof(clen), "%zu", body_len);
    if (n > 0 && (size_t)n < sizeof(clen)) {
        ss_append_header(&h, "Content-Length", 14, clen, (size_t)n);
    }
    ss_append_header(&h, "Connection", 10,
            state->conn->keep_alive ? "keep-alive" : "close",
            state->conn->keep_alive ? 10 : 5);

    state->should_continue = state->conn->keep_alive;
    (void) ss_send_response(state, status_code, &h, body, body_len);
    smart_str_free(&h);

    /* Telemetry — pair on_request_dispatch from hand-off. */
    http_server_count_request(state->conn->counters);
}

/* === Single dispatch callback ===================================== */

static void ss_handle_open(ss_state_t *state, zend_object *exception);
static void ss_handle_stat(ss_state_t *state);
static void ss_handle_sendfile_done(ss_state_t *state);

/* The persistent callback. Registered once at kick-off, fires for
 * every NOTIFY on file_io->event. We discriminate completions by
 * (phase, req identity) — a callback registered during a NOTIFY
 * iteration can re-enter the same iteration, so an unexpected
 * (result, phase) tuple is silently ignored. */
static void ss_dispatch(zend_async_event_t *event,
                        zend_async_event_callback_t *callback,
                        void *result, zend_object *exception)
{
    (void) event;
    ss_state_t *const state = ((ss_cb_t *) callback)->state;
    zend_async_io_req_t *const req = (zend_async_io_req_t *) result;

    switch (state->phase) {
        case SS_PHASE_OPEN:
            /* libuv_fs_open notifies with result=NULL — the only
             * valid signal during this phase. Any non-NULL result is
             * a re-entrant fire from a later phase's submit, ignore. */
            if (req != NULL) return;
            ss_handle_open(state, exception);
            return;

        case SS_PHASE_STAT:
            /* The stat-req identity match guards against a reentrant
             * fire that arrived before our pending_req was set. */
            if (req == NULL || req != state->pending_req) return;
            state->pending_req = NULL;
            if (req->dispose != NULL) req->dispose(req);
            if (UNEXPECTED(exception != NULL)) {
                ss_emit_error(state, 500, "Internal Server Error");
                ss_finalize(state);
                return;
            }
            ss_handle_stat(state);
            return;

        case SS_PHASE_SENDFILE:
            if (req == NULL || req != state->pending_req) return;
            state->pending_req = NULL;
            if (req->dispose != NULL) req->dispose(req);
            ss_handle_sendfile_done(state);
            return;

        case SS_PHASE_DONE:
        default:
            return;
    }
}

/* on_missing:Next rollback (#5c). Open failed on a mount configured to
 * fall through. Tear down the static FSM and hand ctx to a fresh PHP-
 * handler coroutine. Counters and refcounts already bumped in
 * ss_kick_off are left in place — the new coroutine's dispose path will
 * decrement them, balancing the bookkeeping. */
static void ss_rollback_to_php_handler(ss_state_t *state)
{
    http_connection_t   *const conn = state->conn;
    http1_request_ctx_t *const ctx  = state->ctx;

    /* Restore the cork toggle ss_kick_off did. The coroutine path's
     * fire-and-forget writes don't expect cork. Idempotent on non-
     * Linux / non-corked sockets. */
    ss_cork_set(conn, 0);

    /* Drop the file_io + persistent callback (mirrors ss_finalize but
     * without on_request_dispose / http_request_finalize — those happen
     * later via the coroutine's dispose). */
    if (state->cb != NULL && state->file_io != NULL) {
        (void) state->file_io->event.del_callback(
            &state->file_io->event, state->cb);
        state->cb = NULL;
    }
    if (state->file_io != NULL) {
        if (state->file_io->event.dispose != NULL) {
            state->file_io->event.dispose(&state->file_io->event);
        }
        state->file_io = NULL;
    }

    state->phase = SS_PHASE_DONE;

    /* Spawn the PHP handler coroutine (mirrors the dispatch tail in
     * http_connection_dispatch_request).  conn->scope is alive because
     * ss_kick_off pinned conn->handler_refcount. */
    zend_coroutine_t *coroutine = ZEND_ASYNC_NEW_COROUTINE(conn->scope);
    if (UNEXPECTED(coroutine == NULL)) {
        /* Out of memory / scope torn down. Same fallback as the regular
         * dispatch tail: drop ctx and destroy the conn. */
        zval_ptr_dtor(&ctx->request_zv);
        zval_ptr_dtor(&ctx->response_zv);
        efree(ctx);
        ss_state_free(state);
        http_connection_destroy(conn);
        return;
    }

    /* If the static-only deployment had no PHP handler, http_handler_
     * coroutine_entry would dereference NULL conn->handler. Synthesise
     * the same 404 the regular dispatch tail does in that case. */
    if (conn->handler == NULL) {
        http_response_static_set_status(Z_OBJ(ctx->response_zv), 404);
        http_response_static_set_header(Z_OBJ(ctx->response_zv),
            "content-type", 12, "text/plain; charset=utf-8", 25);
        zend_string *msg = zend_string_init("Not Found", 9, 0);
        http_response_static_set_body_str(Z_OBJ(ctx->response_zv), msg);
        zend_string_release(msg);
        ctx->skip_php_handler = true;
    }

    coroutine->internal_entry   = http_handler_coroutine_entry;
    coroutine->extended_data    = ctx;
    coroutine->extended_dispose = http_handler_coroutine_dispose;

    if (ctx->request != NULL) {
        ctx->request->coroutine = coroutine;
    }

    /* The dispatch counter + handler_refcount were already bumped in
     * ss_kick_off. Skip the bump in the regular dispatch tail — the
     * coroutine's dispose still decrements once, balancing the books. */

    ZEND_ASYNC_ENQUEUE_COROUTINE(coroutine);

    /* state lifetime ends here — ctx is owned by the coroutine. */
    ss_state_free(state);
}

static void ss_handle_open(ss_state_t *state, zend_object *exception)
{
    /* libuv flips READABLE on success or CLOSED on error before
     * NOTIFY fires. The exception arg is a redundant signal; either
     * one shipping back to us means the open failed. */
    if (UNEXPECTED(exception != NULL
            || (state->file_io->state & ZEND_ASYNC_IO_CLOSED) != 0)) {
        if (state->mount->flags & HTTP_STATIC_FLAG_ON_MISSING_NEXT) {
            ss_rollback_to_php_handler(state);
            return;
        }
        ss_emit_error(state, 404, "Not Found");
        ss_finalize(state);
        return;
    }

    state->phase       = SS_PHASE_STAT;
    state->pending_req = ZEND_ASYNC_IO_STAT(state->file_io, &state->st);
    if (UNEXPECTED(state->pending_req == NULL)) {
        ss_emit_error(state, 500, "Internal Server Error");
        ss_finalize(state);
    }
}

static void ss_handle_stat(ss_state_t *state)
{
    if (UNEXPECTED(!S_ISREG(state->st.st_mode))) {
        ss_emit_error(state, 404, "Not Found");
        ss_finalize(state);
        return;
    }
    if (UNEXPECTED((uint64_t) state->st.st_size > (uint64_t) HTTP_STATIC_MAX_FILE_SIZE)) {
        ss_emit_error(state, 413, "Payload Too Large");
        ss_finalize(state);
        return;
    }

    char etag_buf[HTTP_STATIC_ETAG_BUF_LEN];
    const bool etag_enabled = (state->mount->flags & HTTP_STATIC_FLAG_ETAG) != 0;
    if (etag_enabled) {
        http_static_etag_format(&state->st, etag_buf);
    }
    char last_modified_buf[HTTP_STATIC_DATE_BUF_LEN];
    http_static_format_http_date(state->st.st_mtime, last_modified_buf);

    const zend_string *if_none_match     = find_request_header(
            state->ctx->request, "if-none-match", 13);
    const zend_string *if_modified_since = find_request_header(
            state->ctx->request, "if-modified-since", 17);
    const bool not_modified = http_static_conditional_match(
            if_none_match     != NULL ? ZSTR_VAL(if_none_match)     : NULL,
            if_none_match     != NULL ? ZSTR_LEN(if_none_match)     : 0,
            if_modified_since != NULL ? ZSTR_VAL(if_modified_since) : NULL,
            if_modified_since != NULL ? ZSTR_LEN(if_modified_since) : 0,
            etag_enabled ? etag_buf : NULL,
            etag_enabled ? HTTP_STATIC_ETAG_LEN : 0,
            state->st.st_mtime);

    const char *content_type = NULL;
    size_t      content_type_len = 0;
    if (!http_static_mime_lookup(state->mount, state->fs_path, state->fs_path_len,
                                 &content_type, &content_type_len)) {
        content_type     = "application/octet-stream";
        content_type_len = sizeof("application/octet-stream") - 1;
    }

    state->should_continue = state->conn->keep_alive;

    smart_str headers = {0};

    if (not_modified) {
        ss_build_headers(state, &headers,
                         content_type, content_type_len,
                         etag_buf, etag_enabled, last_modified_buf,
                         (uint64_t) state->st.st_size, false);
        (void) ss_send_response(state, 304, &headers, NULL, 0);
        smart_str_free(&headers);
        http_server_count_request(state->conn->counters);
        ss_finalize(state);
        return;
    }

    ss_build_headers(state, &headers,
                     content_type, content_type_len,
                     etag_buf, etag_enabled, last_modified_buf,
                     (uint64_t) state->st.st_size, true);

    if (state->is_head || state->st.st_size == 0) {
        (void) ss_send_response(state, 200, &headers, NULL, 0);
        smart_str_free(&headers);
        http_server_count_request(state->conn->counters);
        ss_finalize(state);
        return;
    }

    /* 200 GET with body. Headers go fire-and-forget through the
     * existing send pipeline (plain TCP only on this path — TLS would
     * require a non-suspending TLS write helper); the body rides the
     * zero-copy sendfile syscall. */
    if (UNEXPECTED(!ss_send_response(state, 200, &headers, NULL, 0))) {
        smart_str_free(&headers);
        ss_finalize(state);
        return;
    }
    smart_str_free(&headers);

    state->phase       = SS_PHASE_SENDFILE;
    state->pending_req = ZEND_ASYNC_IO_SENDFILE(
            state->conn->io, state->file_io, 0, (size_t) state->st.st_size);
    if (UNEXPECTED(state->pending_req == NULL)) {
        http_server_count_request(state->conn->counters);
        ss_finalize(state);
    }
}

static void ss_handle_sendfile_done(ss_state_t *state)
{
    /* Body sent (or partially sent on error). On error we still
     * finalize — bytes already on the wire are out of our control. */
    http_server_count_request(state->conn->counters);
    ss_finalize(state);
}

/* === Hard-zero kick-off =========================================== */

/* Hard-zero is plain-TCP-only for now: the fire-and-forget write
 * path (http_connection_send_str_owned) writes directly to the
 * socket fd, bypassing the user-space TLS layer. A future iteration
 * can add a non-suspending TLS write helper to extend this to
 * kTLS-engaged sessions. */
#ifdef HAVE_OPENSSL
static inline bool conn_supports_sendfile(const http_connection_t *conn)
{
    return conn->tls == NULL;
}
#else
static inline bool conn_supports_sendfile(const http_connection_t *conn)
{
    (void) conn;
    return true;
}
#endif

/* Take the dispatch hand-off and start the async chain. Returns true
 * on success — caller must return HARD_ZERO. False = setup failed,
 * caller falls back to the synchronous-populate path. */
static bool ss_kick_off(http_connection_t *conn, http1_request_ctx_t *ctx,
                        const http_static_handler_t *mount,
                        const char *fs_path, size_t fs_path_len,
                        const bool is_head)
{
    if (UNEXPECTED(fs_path_len + 1 >= PATH_MAX)) {
        return false;
    }

    ss_state_t *state = ecalloc(1, sizeof(*state));
    state->conn       = conn;
    state->ctx        = ctx;
    state->mount      = mount;
    state->is_head    = is_head;
    state->fs_path    = emalloc(fs_path_len + 1);
    memcpy(state->fs_path, fs_path, fs_path_len);
    state->fs_path[fs_path_len] = '\0';
    state->fs_path_len = fs_path_len;

    state->phase = SS_PHASE_OPEN;

    state->file_io = ZEND_ASYNC_FS_OPEN(state->fs_path, O_RDONLY | O_CLOEXEC, 0);
    if (UNEXPECTED(state->file_io == NULL)) {
        ss_state_free(state);
        return false;
    }

    /* One persistent callback for the whole chain. */
    ss_cb_t *cb = (ss_cb_t *)
        ZEND_ASYNC_EVENT_CALLBACK_EX(ss_dispatch, sizeof(ss_cb_t));
    if (UNEXPECTED(cb == NULL)) {
        if (state->file_io->event.dispose != NULL) {
            state->file_io->event.dispose(&state->file_io->event);
        }
        ss_state_free(state);
        return false;
    }
    cb->base.dispose = ss_cb_dispose;
    cb->state        = state;

    if (UNEXPECTED(!state->file_io->event.add_callback(
            &state->file_io->event, &cb->base))) {
        efree(cb);
        if (state->file_io->event.dispose != NULL) {
            state->file_io->event.dispose(&state->file_io->event);
        }
        ss_state_free(state);
        return false;
    }
    state->cb = &cb->base;

    /* Pin the conn for the duration of the chain — paired with
     * http_request_finalize's --refcount inside ss_finalize. */
    conn->handler_refcount++;
    conn->state = CONN_STATE_PROCESSING;
    http_server_on_request_dispatch(conn->counters);

    /* Telemetry — see http_server_counters_t::static_zero_coroutine_total.
     * Counted on commit (we hold the refcount), not on completion: the
     * sendfile may still ENOENT-rollback for on_missing:Next mounts, but
     * the kick-off itself is the metric's anchor (cache-friendly path
     * was selected). */
    http_server_on_static_zero_coroutine(conn->counters);

    /* Cork now so the headers write (about to be queued from
     * ss_handle_stat) and the subsequent sendfile bytes coalesce on
     * the wire. Uncorked unconditionally in ss_finalize. */
    ss_cork_set(conn, 1);

    return true;
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

    /* GET/HEAD only — operators can overlay POST/PUT endpoints on the
     * same prefix without the static layer turning them into 405s. */
    const bool is_head = method_is_head(request);
    const bool is_get  = method_is_get(request);
    if (!is_get && !is_head) {
        return HTTP_STATIC_PASSTHROUGH;
    }

    /* request->path is built lazily by the PHP-side getter; req->uri is
     * always populated by the parser. http_static_path_resolve strips
     * '?' and '#' so the whole URI is safe to feed in. */
    const char  *const req_path     = (request->uri != NULL) ? ZSTR_VAL(request->uri) : NULL;
    const size_t       req_path_len = (request->uri != NULL) ? ZSTR_LEN(request->uri) : 0;
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
            continue;
        }
        if (UNEXPECTED(rc == HTTP_STATIC_PATH_BAD_REQUEST)) {
            emit_status(response_obj, 400, "Bad Request", 11);
            ctx->skip_php_handler = true;
            return HTTP_STATIC_HANDLED;
        }
        /* Dotfile-deny / traversal escape: 404 (not 403) so existence
         * of the restricted resource isn't disclosed. */
        if (UNEXPECTED(rc == HTTP_STATIC_PATH_FORBIDDEN)) {
            emit_status(response_obj, 404, "Not Found", 9);
            ctx->skip_php_handler = true;
            return HTTP_STATIC_HANDLED;
        }
        if (UNEXPECTED(rc == HTTP_STATIC_PATH_HIDE)) {
            if (mount->flags & HTTP_STATIC_FLAG_ON_MISSING_NEXT) {
                return HTTP_STATIC_PASSTHROUGH;
            }
            emit_status(response_obj, 404, "Not Found", 9);
            ctx->skip_php_handler = true;
            return HTTP_STATIC_HANDLED;
        }

        /* Hide-globs match against the relative path so operator-
         * authored patterns target what they see. */
        if (UNEXPECTED(relative_len > 0
                && http_static_path_is_hidden(mount, relative, relative_len))) {
            if (mount->flags & HTTP_STATIC_FLAG_ON_MISSING_NEXT) {
                return HTTP_STATIC_PASSTHROUGH;
            }
            emit_status(response_obj, 404, "Not Found", 9);
            ctx->skip_php_handler = true;
            return HTTP_STATIC_HANDLED;
        }

        /* Resolve directory→index synchronously before the hard-zero
         * gate (#5b in TODO_STATIC_HANDLER_REVIEW). Cold-cache stat is
         * a single inode lookup (microseconds); it's the read+send that
         * we want async, and that's what hard-zero already gives us.
         *
         * On hit, fs_path_len is promoted to the joined path so MIME
         * lookup sees the index file's extension (.html etc) rather
         * than the directory's. On miss, the request resolves to 404
         * uniformly — on_missing:Next mounts return PASSTHROUGH so the
         * PHP handler can take over. */
        const bool was_directory = path_targets_directory(relative, relative_len);
        if (was_directory) {
            bool index_resolved = false;
            for (size_t ii = 0; ii < mount->index_count; ii++) {
                const zend_string *const idx = mount->index_files[ii];
                size_t cand_len = fs_path_len;
                if (UNEXPECTED(!http_static_path_join(fs_path, sizeof(fs_path),
                                                      &cand_len,
                                                      ZSTR_VAL(idx),
                                                      ZSTR_LEN(idx)))) {
                    continue;
                }
                struct stat sb;
                if (stat(fs_path, &sb) == 0 && S_ISREG(sb.st_mode)) {
                    fs_path_len    = cand_len;
                    index_resolved = true;
                    break;
                }
                /* Truncate back so the next candidate starts from the
                 * pristine directory prefix. */
                fs_path[fs_path_len] = '\0';
            }
            if (!index_resolved) {
                if (mount->flags & HTTP_STATIC_FLAG_ON_MISSING_NEXT) {
                    return HTTP_STATIC_PASSTHROUGH;
                }
                emit_status(response_obj, 404, "Not Found", 9);
                ctx->skip_php_handler = true;
                return HTTP_STATIC_HANDLED;
            }
        }

        /* Hard-zero async path — eligible when:
         *  - destination socket can take zero-copy writes (plain TCP),
         *  - the resolved path stays inside the mount root after
         *    canonicalisation (sync realpath here, microseconds on
         *    warm cache; sendfile/sync paths share the same security
         *    check this way).
         * On open-error the on_missing:Next rollback in ss_handle_open
         * detaches state and hands ctx over to a regular PHP-handler
         * coroutine, so on_missing:Next mounts now ride hard-zero on
         * the success path too (#5c). */
        if (conn_supports_sendfile(conn)
            && resolved_under_root(mount, fs_path)) {
            if (ss_kick_off(conn, ctx, mount, fs_path, fs_path_len, is_head)) {
                return HTTP_STATIC_HARD_ZERO;
            }
        }

        int fd = -1;
        struct stat st;
        bool opened = try_open_candidate(mount, fs_path, &fd, &st);

        if (UNEXPECTED(!opened)) {
            /* ENOENT / ELOOP (symlink rejected) / ENOTDIR / EACCES /
             * EPERM all collapse to "not available". 404 (rather than
             * 403 on EACCES) avoids disclosing whether a restricted
             * file actually exists, matching the dotfile-deny path. */
            if (mount->flags & HTTP_STATIC_FLAG_ON_MISSING_NEXT) {
                return HTTP_STATIC_PASSTHROUGH;
            }
            emit_status(response_obj, 404, "Not Found", 9);
            ctx->skip_php_handler = true;
            return HTTP_STATIC_HANDLED;
        }

        /* Closes the intermediate-symlink-traversal gap that O_NOFOLLOW
         * leaves open: realpath() canonicalises every segment, so a
         * symlink anywhere on the path that points outside the mount
         * surfaces here as a prefix mismatch. */
        if (UNEXPECTED(!resolved_under_root(mount, fs_path))) {
            close(fd);
            emit_status(response_obj, 404, "Not Found", 9);
            ctx->skip_php_handler = true;
            return HTTP_STATIC_HANDLED;
        }

        /* Synchronous slurp cap — protects the loop from a stray giant
         * file. PR #5 (sendfile / async) removes the limit. */
        if (UNEXPECTED((uint64_t)st.st_size > (uint64_t)HTTP_STATIC_MAX_FILE_SIZE)) {
            close(fd);
            emit_status(response_obj, 413, "Payload Too Large", 17);
            ctx->skip_php_handler = true;
            return HTTP_STATIC_HANDLED;
        }

        char etag_buf[HTTP_STATIC_ETAG_BUF_LEN];
        const bool etag_enabled = (mount->flags & HTTP_STATIC_FLAG_ETAG) != 0;
        if (etag_enabled) {
            http_static_etag_format(&st, etag_buf);
        }

        char last_modified_buf[HTTP_STATIC_DATE_BUF_LEN];
        http_static_format_http_date(st.st_mtime, last_modified_buf);

        const zend_string *const if_none_match     =
            find_request_header(request, "if-none-match", 13);
        const zend_string *const if_modified_since =
            find_request_header(request, "if-modified-since", 17);
        const bool not_modified = http_static_conditional_match(
            if_none_match     != NULL ? ZSTR_VAL(if_none_match)     : NULL,
            if_none_match     != NULL ? ZSTR_LEN(if_none_match)     : 0,
            if_modified_since != NULL ? ZSTR_VAL(if_modified_since) : NULL,
            if_modified_since != NULL ? ZSTR_LEN(if_modified_since) : 0,
            etag_enabled ? etag_buf : NULL,
            etag_enabled ? HTTP_STATIC_ETAG_LEN : 0,
            st.st_mtime);

        if (not_modified) {
            close(fd);
            http_response_static_set_status(response_obj, 304);
            if (etag_enabled) {
                http_response_static_set_header(response_obj,
                    "etag", 4, etag_buf, HTTP_STATIC_ETAG_LEN);
            }
            http_response_static_set_header(response_obj,
                "last-modified", 13,
                last_modified_buf, HTTP_STATIC_DATE_LEN);
            if (mount->cache_control != NULL) {
                http_response_static_set_header(response_obj,
                    "cache-control", 13,
                    ZSTR_VAL(mount->cache_control),
                    ZSTR_LEN(mount->cache_control));
            }
            apply_extra_headers(response_obj, mount, false);
            ctx->skip_php_handler = true;
            return HTTP_STATIC_HANDLED;
        }

        zend_string *body = NULL;
        if (is_get) {
            body = slurp_fd(fd, (size_t)st.st_size);
            if (UNEXPECTED(body == NULL)) {
                close(fd);
                emit_status(response_obj, 500, "Internal Server Error", 21);
                ctx->skip_php_handler = true;
                return HTTP_STATIC_HANDLED;
            }
        }
        close(fd);

        http_response_static_set_status(response_obj, 200);

        const char *content_type     = NULL;
        size_t      content_type_len = 0;
        if (!http_static_mime_lookup(mount, fs_path, fs_path_len,
                                     &content_type, &content_type_len)) {
            content_type     = "application/octet-stream";
            content_type_len = sizeof("application/octet-stream") - 1;
        }
        http_response_static_set_header(response_obj,
            "content-type", 12, content_type, content_type_len);

        if (etag_enabled) {
            http_response_static_set_header(response_obj,
                "etag", 4, etag_buf, HTTP_STATIC_ETAG_LEN);
        }
        http_response_static_set_header(response_obj,
            "last-modified", 13,
            last_modified_buf, HTTP_STATIC_DATE_LEN);
        if (mount->cache_control != NULL) {
            http_response_static_set_header(response_obj,
                "cache-control", 13,
                ZSTR_VAL(mount->cache_control),
                ZSTR_LEN(mount->cache_control));
        }
        apply_extra_headers(response_obj, mount, true);

        if (is_head) {
            /* The format-time path computes Content-Length from the
             * body smart_str; with an empty body we have to advertise
             * the would-be size explicitly. */
            char clen[32];
            const int n = snprintf(clen, sizeof(clen), "%" PRIu64,
                                   (uint64_t)st.st_size);
            if (EXPECTED(n > 0 && (size_t)n < sizeof(clen))) {
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
