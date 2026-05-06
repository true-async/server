/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

/* StaticHandler — built-in static file handler (issue #13).
 *
 * One StaticHandler describes one URL-prefix-rooted static mount.
 * Attached to an HttpServer via HttpServer::addStaticHandler(), at
 * which point it transitions to "locked" — all setters reject
 * further mutation. The dispatch hook in http_connection.c walks
 * the locked array during request dispatch and decides whether to
 * serve in C or fall through to the PHP handler. */

#ifndef TRUE_ASYNC_STATIC_HANDLER_H
#define TRUE_ASYNC_STATIC_HANDLER_H

#include "php.h"
#include "zend_smart_str.h"
#include <stdbool.h>
#include <stdint.h>

/* Behaviour bits packed into http_static_handler_t::flags. One cache-line
 * load on the dispatch fast path; no per-bool loads. */
#define HTTP_STATIC_FLAG_DOTFILES_DENY        (1u << 0)  /* default */
#define HTTP_STATIC_FLAG_DOTFILES_ALLOW       (1u << 1)
#define HTTP_STATIC_FLAG_DOTFILES_IGNORE      (1u << 2)
#define HTTP_STATIC_FLAG_SYMLINKS_REJECT      (1u << 3)  /* default */
#define HTTP_STATIC_FLAG_SYMLINKS_FOLLOW      (1u << 4)
#define HTTP_STATIC_FLAG_SYMLINKS_OWNER       (1u << 5)
#define HTTP_STATIC_FLAG_PRECOMP_BR           (1u << 6)
#define HTTP_STATIC_FLAG_PRECOMP_GZIP         (1u << 7)
#define HTTP_STATIC_FLAG_PRECOMP_ZSTD         (1u << 8)
#define HTTP_STATIC_FLAG_ETAG                 (1u << 9)  /* default on */
#define HTTP_STATIC_FLAG_BROWSE               (1u << 10)
#define HTTP_STATIC_FLAG_ON_MISSING_NEXT      (1u << 11)
#define HTTP_STATIC_FLAG_LOCKED               (1u << 12)

/* Persistent mount descriptor. One copy lives inside each StaticHandler
 * PHP object; addStaticHandler() locks the object and the server stores
 * a pointer to the same struct. The array on the server is therefore
 * read-only after start().
 *
 * Strings are zend_strings (refcounted, may be persistent or not — every
 * setter takes its own ref). hide_globs / index_files are heap arrays of
 * refcounted zend_strings. extra_headers / mime_overrides are HashTables
 * (pointer-stable across pre-lock mutations). */
typedef struct {
    /* URL prefix (always starts and ends with '/') and its length, kept
     * separately so the dispatch fast path can do a single memcmp. */
    zend_string  *url_prefix;
    size_t        url_prefix_len;

    /* Filesystem root, canonicalised at attach time. */
    zend_string  *root_directory;

    /* Optional pre-formatted Cache-Control header value. NULL = no header. */
    zend_string  *cache_control;

    /* Index file candidates. NULL pointer + index_count == 0 means
     * "directory request → 404 / passthrough". */
    zend_string **index_files;
    size_t        index_count;

    /* Hide-glob array. Matched against the path RELATIVE to root. */
    zend_string **hide_globs;
    size_t        hide_count;

    /* Extra response headers (set via setHeader). HashTable<lower-name,
     * IS_STRING value>. NULL until the first setHeader call. */
    HashTable    *extra_headers;

    /* Per-mount MIME overrides: HashTable<lower-extension, IS_STRING
     * content-type>. NULL until the first setMimeType call. The built-in
     * MIME table is consulted first; this is only the fallback. */
    HashTable    *mime_overrides;

    uint32_t      flags;
} http_static_handler_t;

/* Result of the dispatch attempt. HANDLED → C handler owns the
 * connection lifecycle from here. ERROR → C handler emitted (or will
 * emit) a short error response on its own. PASSTHROUGH → no static
 * mount matched (or `on_missing: Next` fell through); the dispatcher
 * must continue with the regular coroutine + PHP handler path. */
typedef enum {
    HTTP_STATIC_PASSTHROUGH = 0,
    HTTP_STATIC_HANDLED     = 1,
    HTTP_STATIC_ERROR       = 2,
} http_static_result_t;

/* Forward decls — concrete types live in C TUs that include this. */
struct _http_request_t;
struct _http_connection_t;
struct http_server_object;

/* PHP class entries — filled in by the static_handler_class_register()
 * call from MINIT. */
extern zend_class_entry *http_static_handler_ce;
extern zend_class_entry *http_static_on_missing_ce;
extern zend_class_entry *http_static_dotfiles_ce;
extern zend_class_entry *http_static_symlinks_ce;

/* MINIT entrypoint. */
void http_static_handler_class_register(void);

/* Pull the underlying mount descriptor out of a PHP StaticHandler
 * object. NULL if the zend_object isn't a StaticHandler. */
http_static_handler_t *http_static_handler_from_obj(zend_object *obj);

/* Lock the handler (called from HttpServer::addStaticHandler before the
 * descriptor pointer is stored on the server). After locking, every
 * setter throws HttpServerRuntimeException. */
void http_static_handler_lock(http_static_handler_t *handler);

/* Dispatch hook entrypoint — declared here so http_connection.c can
 * call it without needing the full implementation header. The current
 * skeleton always returns PASSTHROUGH; the real FSM lands incrementally
 * (PRs #1+ per docs/PLAN_STATIC_HANDLER.md). */
http_static_result_t http_static_try_serve(struct http_server_object *server,
                                           struct _http_connection_t *conn,
                                           void *ctx,
                                           struct _http_request_t *request);

#endif /* TRUE_ASYNC_STATIC_HANDLER_H */
