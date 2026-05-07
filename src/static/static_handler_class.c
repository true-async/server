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
#include "zend_exceptions.h"
#include "Zend/zend_enum.h"
#include "Zend/zend_virtual_cwd.h"
#include "php_http_server.h"
#include "static/static_handler.h"

#include <stdint.h>
#include <sys/stat.h>

#include "../../stubs/StaticHandler.php_arginfo.h"

zend_class_entry *http_static_handler_ce;
zend_class_entry *http_static_on_missing_ce;
zend_class_entry *http_static_dotfiles_ce;
zend_class_entry *http_static_symlinks_ce;

static zend_object_handlers http_static_handler_object_handlers;

/* Embed the descriptor inside the zend_object so a single PHP object
 * carries both the user-facing handle and the persistent mount config. */
typedef struct {
    http_static_handler_t mount;
    zend_object           std;
} http_static_handler_php_t;

static inline http_static_handler_php_t *http_static_handler_php_from_obj(zend_object *obj)
{
    return (http_static_handler_php_t *)((char *)obj
        - XtOffsetOf(http_static_handler_php_t, std));
}

http_static_handler_t *http_static_handler_from_obj(zend_object *obj)
{
    if (obj == NULL || obj->ce != http_static_handler_ce) {
        return NULL;
    }
    return &http_static_handler_php_from_obj(obj)->mount;
}

#define Z_HTTP_STATIC_HANDLER_P(zv) http_static_handler_from_obj(Z_OBJ_P(zv))

void http_static_handler_lock(http_static_handler_t *handler)
{
    if (handler != NULL) {
        handler->flags |= HTTP_STATIC_FLAG_LOCKED;
    }
}

/* http_static_try_serve is implemented in http_static_serve.c. */

static inline bool handler_check_locked(const http_static_handler_t *mount)
{
    if (UNEXPECTED(mount->flags & HTTP_STATIC_FLAG_LOCKED)) {
        zend_throw_exception(http_server_runtime_exception_ce,
            "Cannot modify StaticHandler after attachment to HttpServer", 0);
        return true;
    }
    return false;
}

/* Reject prefixes that don't bracket with '/', contain NUL, or
 * collapse to "//" — the dispatch fast path memcmps assuming both
 * the request URL and the prefix are well-formed. */
static bool validate_url_prefix(const zend_string *prefix)
{
    const size_t len = ZSTR_LEN(prefix);
    const char *const val = ZSTR_VAL(prefix);

    if (len < 2 || val[0] != '/' || val[len - 1] != '/') {
        zend_throw_exception(http_server_invalid_argument_exception_ce,
            "StaticHandler url prefix must start and end with '/'", 0);
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        if (val[i] == '\0') {
            zend_throw_exception(http_server_invalid_argument_exception_ce,
                "StaticHandler url prefix must not contain NUL", 0);
            return false;
        }
        if (i + 1 < len && val[i] == '/' && val[i + 1] == '/') {
            zend_throw_exception(http_server_invalid_argument_exception_ce,
                "StaticHandler url prefix must not contain '//'", 0);
            return false;
        }
    }
    return true;
}

/* Validate root directory: absolute, exists, is a directory. The
 * canonical (realpath) form is computed at attach time, not here —
 * setter-time checks are about diagnostics, not security. */
static zend_string *canonicalise_root_directory(const zend_string *path)
{
    if (ZSTR_LEN(path) == 0) {
        zend_throw_exception(http_server_invalid_argument_exception_ce,
            "StaticHandler root directory must not be empty", 0);
        return NULL;
    }
    if (ZSTR_VAL(path)[0] != '/') {
        zend_throw_exception(http_server_invalid_argument_exception_ce,
            "StaticHandler root directory must be an absolute path", 0);
        return NULL;
    }

    zend_stat_t sb;
    if (VCWD_STAT(ZSTR_VAL(path), &sb) != 0) {
        zend_throw_exception_ex(http_server_invalid_argument_exception_ce, 0,
            "StaticHandler root directory not found: %s", ZSTR_VAL(path));
        return NULL;
    }
    if (!S_ISDIR(sb.st_mode)) {
        zend_throw_exception_ex(http_server_invalid_argument_exception_ce, 0,
            "StaticHandler root directory is not a directory: %s",
            ZSTR_VAL(path));
        return NULL;
    }

    char resolved[MAXPATHLEN];
    if (VCWD_REALPATH(ZSTR_VAL(path), resolved) == NULL) {
        zend_throw_exception_ex(http_server_invalid_argument_exception_ce, 0,
            "StaticHandler root directory cannot be canonicalised: %s",
            ZSTR_VAL(path));
        return NULL;
    }
    return zend_string_init(resolved, strlen(resolved), 0);
}

static void mount_release_owned(http_static_handler_t *mount)
{
    if (mount->url_prefix != NULL) {
        zend_string_release(mount->url_prefix);
        mount->url_prefix = NULL;
    }
    if (mount->root_directory != NULL) {
        zend_string_release(mount->root_directory);
        mount->root_directory = NULL;
    }
    if (mount->cache_control != NULL) {
        zend_string_release(mount->cache_control);
        mount->cache_control = NULL;
    }
    if (mount->index_files != NULL) {
        for (size_t i = 0; i < mount->index_count; i++) {
            zend_string_release(mount->index_files[i]);
        }
        efree(mount->index_files);
        mount->index_files = NULL;
        mount->index_count = 0;
    }
    if (mount->hide_globs != NULL) {
        for (size_t i = 0; i < mount->hide_count; i++) {
            zend_string_release(mount->hide_globs[i]);
        }
        efree(mount->hide_globs);
        mount->hide_globs = NULL;
        mount->hide_count = 0;
    }
    if (mount->extra_headers != NULL) {
        zend_hash_destroy(mount->extra_headers);
        FREE_HASHTABLE(mount->extra_headers);
        mount->extra_headers = NULL;
    }
    if (mount->mime_overrides != NULL) {
        zend_hash_destroy(mount->mime_overrides);
        FREE_HASHTABLE(mount->mime_overrides);
        mount->mime_overrides = NULL;
    }
}

ZEND_METHOD(TrueAsync_StaticHandler, __construct)
{
    zend_string *url_prefix = NULL;
    zend_string *root_directory = NULL;

    ZEND_PARSE_PARAMETERS_START(2, 2)
        Z_PARAM_STR(url_prefix)
        Z_PARAM_STR(root_directory)
    ZEND_PARSE_PARAMETERS_END();

    http_static_handler_t *mount = Z_HTTP_STATIC_HANDLER_P(ZEND_THIS);

    if (!validate_url_prefix(url_prefix)) {
        return;
    }
    zend_string *root = canonicalise_root_directory(root_directory);
    if (root == NULL) {
        return;
    }

    mount->url_prefix     = zend_string_copy(url_prefix);
    mount->url_prefix_len = ZSTR_LEN(url_prefix);
    mount->root_directory = root;

    mount->index_files    = emalloc(sizeof(zend_string *));
    mount->index_files[0] = zend_string_init(ZEND_STRL("index.html"), 0);
    mount->index_count    = 1;

    mount->flags = HTTP_STATIC_FLAG_DOTFILES_DENY
                 | HTTP_STATIC_FLAG_SYMLINKS_REJECT
                 | HTTP_STATIC_FLAG_ETAG;
}

ZEND_METHOD(TrueAsync_StaticHandler, setIndexFiles)
{
    zval *args = NULL;
    uint32_t argc = 0;

    ZEND_PARSE_PARAMETERS_START(0, -1)
        Z_PARAM_VARIADIC('+', args, argc)
    ZEND_PARSE_PARAMETERS_END();

    http_static_handler_t *mount = Z_HTTP_STATIC_HANDLER_P(ZEND_THIS);
    if (handler_check_locked(mount)) {
        return;
    }

    /* Validate up-front so we never half-update the index list. */
    for (uint32_t i = 0; i < argc; i++) {
        if (Z_TYPE(args[i]) != IS_STRING) {
            zend_throw_exception(http_server_invalid_argument_exception_ce,
                "StaticHandler index files must be strings", 0);
            return;
        }
        const zend_string *s = Z_STR(args[i]);
        if (ZSTR_LEN(s) == 0) {
            zend_throw_exception(http_server_invalid_argument_exception_ce,
                "StaticHandler index file name must not be empty", 0);
            return;
        }
        if (memchr(ZSTR_VAL(s), '/', ZSTR_LEN(s)) != NULL) {
            zend_throw_exception_ex(http_server_invalid_argument_exception_ce, 0,
                "StaticHandler index file name must not contain '/': %s",
                ZSTR_VAL(s));
            return;
        }
    }

    if (mount->index_files != NULL) {
        for (size_t i = 0; i < mount->index_count; i++) {
            zend_string_release(mount->index_files[i]);
        }
        efree(mount->index_files);
        mount->index_files = NULL;
        mount->index_count = 0;
    }

    if (argc > 0) {
        mount->index_files = emalloc(sizeof(zend_string *) * argc);
        for (uint32_t i = 0; i < argc; i++) {
            mount->index_files[i] = zend_string_copy(Z_STR(args[i]));
        }
        mount->index_count = argc;
    }

    RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
}

ZEND_METHOD(TrueAsync_StaticHandler, disableIndex)
{
    ZEND_PARSE_PARAMETERS_NONE();
    http_static_handler_t *mount = Z_HTTP_STATIC_HANDLER_P(ZEND_THIS);
    if (handler_check_locked(mount)) {
        return;
    }

    if (mount->index_files != NULL) {
        for (size_t i = 0; i < mount->index_count; i++) {
            zend_string_release(mount->index_files[i]);
        }
        efree(mount->index_files);
        mount->index_files = NULL;
        mount->index_count = 0;
    }

    RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
}

ZEND_METHOD(TrueAsync_StaticHandler, setOnMissing)
{
    zval *mode_zv = NULL;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_OBJECT_OF_CLASS(mode_zv, http_static_on_missing_ce)
    ZEND_PARSE_PARAMETERS_END();

    http_static_handler_t *mount = Z_HTTP_STATIC_HANDLER_P(ZEND_THIS);
    if (handler_check_locked(mount)) {
        return;
    }

    zval *value = zend_enum_fetch_case_value(Z_OBJ_P(mode_zv));
    const zend_long mode = Z_LVAL_P(value);

    if (mode == 1) {
        mount->flags |= HTTP_STATIC_FLAG_ON_MISSING_NEXT;
    } else {
        mount->flags &= ~HTTP_STATIC_FLAG_ON_MISSING_NEXT;
    }

    RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
}

ZEND_METHOD(TrueAsync_StaticHandler, enablePrecompressed)
{
    zval *args = NULL;
    uint32_t argc = 0;

    ZEND_PARSE_PARAMETERS_START(0, -1)
        Z_PARAM_VARIADIC('+', args, argc)
    ZEND_PARSE_PARAMETERS_END();

    http_static_handler_t *mount = Z_HTTP_STATIC_HANDLER_P(ZEND_THIS);
    if (handler_check_locked(mount)) {
        return;
    }

    uint32_t enabled_bits = 0;
    for (uint32_t i = 0; i < argc; i++) {
        if (Z_TYPE(args[i]) != IS_STRING) {
            zend_throw_exception(http_server_invalid_argument_exception_ce,
                "StaticHandler precompressed encoding name must be a string", 0);
            return;
        }
        const zend_string *s = Z_STR(args[i]);
        if (zend_string_equals_literal(s, "br")) {
            enabled_bits |= HTTP_STATIC_FLAG_PRECOMP_BR;
        } else if (zend_string_equals_literal(s, "gzip")) {
            enabled_bits |= HTTP_STATIC_FLAG_PRECOMP_GZIP;
        } else if (zend_string_equals_literal(s, "zstd")) {
            enabled_bits |= HTTP_STATIC_FLAG_PRECOMP_ZSTD;
        } else {
            zend_throw_exception_ex(http_server_invalid_argument_exception_ce, 0,
                "StaticHandler unknown precompressed encoding '%s' "
                "(expected one of: br, gzip, zstd)", ZSTR_VAL(s));
            return;
        }
    }

    mount->flags |= enabled_bits;

    RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
}

ZEND_METHOD(TrueAsync_StaticHandler, disablePrecompressed)
{
    ZEND_PARSE_PARAMETERS_NONE();
    http_static_handler_t *mount = Z_HTTP_STATIC_HANDLER_P(ZEND_THIS);
    if (handler_check_locked(mount)) {
        return;
    }

    mount->flags &= ~(HTTP_STATIC_FLAG_PRECOMP_BR
                    | HTTP_STATIC_FLAG_PRECOMP_GZIP
                    | HTTP_STATIC_FLAG_PRECOMP_ZSTD);

    RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
}

ZEND_METHOD(TrueAsync_StaticHandler, setDotfilePolicy)
{
    zval *policy_zv = NULL;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_OBJECT_OF_CLASS(policy_zv, http_static_dotfiles_ce)
    ZEND_PARSE_PARAMETERS_END();

    http_static_handler_t *mount = Z_HTTP_STATIC_HANDLER_P(ZEND_THIS);
    if (handler_check_locked(mount)) {
        return;
    }

    mount->flags &= ~(HTTP_STATIC_FLAG_DOTFILES_DENY
                    | HTTP_STATIC_FLAG_DOTFILES_ALLOW
                    | HTTP_STATIC_FLAG_DOTFILES_IGNORE);

    zval *value = zend_enum_fetch_case_value(Z_OBJ_P(policy_zv));
    switch (Z_LVAL_P(value)) {
        case 0: mount->flags |= HTTP_STATIC_FLAG_DOTFILES_DENY;   break;
        case 1: mount->flags |= HTTP_STATIC_FLAG_DOTFILES_ALLOW;  break;
        case 2: mount->flags |= HTTP_STATIC_FLAG_DOTFILES_IGNORE; break;
        default: mount->flags |= HTTP_STATIC_FLAG_DOTFILES_DENY;  break;
    }

    RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
}

ZEND_METHOD(TrueAsync_StaticHandler, setSymlinkPolicy)
{
    zval *policy_zv = NULL;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_OBJECT_OF_CLASS(policy_zv, http_static_symlinks_ce)
    ZEND_PARSE_PARAMETERS_END();

    http_static_handler_t *mount = Z_HTTP_STATIC_HANDLER_P(ZEND_THIS);
    if (handler_check_locked(mount)) {
        return;
    }

    mount->flags &= ~(HTTP_STATIC_FLAG_SYMLINKS_REJECT
                    | HTTP_STATIC_FLAG_SYMLINKS_FOLLOW
                    | HTTP_STATIC_FLAG_SYMLINKS_OWNER);

    zval *value = zend_enum_fetch_case_value(Z_OBJ_P(policy_zv));
    switch (Z_LVAL_P(value)) {
        case 0: mount->flags |= HTTP_STATIC_FLAG_SYMLINKS_REJECT; break;
        case 1: mount->flags |= HTTP_STATIC_FLAG_SYMLINKS_FOLLOW; break;
        case 2: mount->flags |= HTTP_STATIC_FLAG_SYMLINKS_OWNER;  break;
        default: mount->flags |= HTTP_STATIC_FLAG_SYMLINKS_REJECT; break;
    }

    RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
}

ZEND_METHOD(TrueAsync_StaticHandler, hide)
{
    zval *args = NULL;
    uint32_t argc = 0;

    ZEND_PARSE_PARAMETERS_START(0, -1)
        Z_PARAM_VARIADIC('+', args, argc)
    ZEND_PARSE_PARAMETERS_END();

    http_static_handler_t *mount = Z_HTTP_STATIC_HANDLER_P(ZEND_THIS);
    if (handler_check_locked(mount)) {
        return;
    }

    for (uint32_t i = 0; i < argc; i++) {
        if (Z_TYPE(args[i]) != IS_STRING) {
            zend_throw_exception(http_server_invalid_argument_exception_ce,
                "StaticHandler hide pattern must be a string", 0);
            return;
        }
        if (Z_STRLEN(args[i]) == 0) {
            zend_throw_exception(http_server_invalid_argument_exception_ce,
                "StaticHandler hide pattern must not be empty", 0);
            return;
        }
    }

    const size_t new_count = mount->hide_count + argc;
    if (new_count > 0) {
        mount->hide_globs = mount->hide_globs
            ? erealloc(mount->hide_globs, sizeof(zend_string *) * new_count)
            : emalloc(sizeof(zend_string *) * new_count);
        for (uint32_t i = 0; i < argc; i++) {
            mount->hide_globs[mount->hide_count + i] =
                zend_string_copy(Z_STR(args[i]));
        }
        mount->hide_count = new_count;
    }

    RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
}

ZEND_METHOD(TrueAsync_StaticHandler, setEtagEnabled)
{
    bool enabled;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_BOOL(enabled)
    ZEND_PARSE_PARAMETERS_END();

    http_static_handler_t *mount = Z_HTTP_STATIC_HANDLER_P(ZEND_THIS);
    if (handler_check_locked(mount)) {
        return;
    }

    if (enabled) {
        mount->flags |= HTTP_STATIC_FLAG_ETAG;
    } else {
        mount->flags &= ~HTTP_STATIC_FLAG_ETAG;
    }

    RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
}

ZEND_METHOD(TrueAsync_StaticHandler, setCacheControl)
{
    zend_string *value = NULL;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_STR(value)
    ZEND_PARSE_PARAMETERS_END();

    http_static_handler_t *mount = Z_HTTP_STATIC_HANDLER_P(ZEND_THIS);
    if (handler_check_locked(mount)) {
        return;
    }

    if (mount->cache_control != NULL) {
        zend_string_release(mount->cache_control);
        mount->cache_control = NULL;
    }
    if (ZSTR_LEN(value) > 0) {
        mount->cache_control = zend_string_copy(value);
    }

    RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
}

ZEND_METHOD(TrueAsync_StaticHandler, setHeader)
{
    zend_string *name = NULL;
    zend_string *value = NULL;

    ZEND_PARSE_PARAMETERS_START(2, 2)
        Z_PARAM_STR(name)
        Z_PARAM_STR(value)
    ZEND_PARSE_PARAMETERS_END();

    http_static_handler_t *mount = Z_HTTP_STATIC_HANDLER_P(ZEND_THIS);
    if (handler_check_locked(mount)) {
        return;
    }

    if (ZSTR_LEN(name) == 0) {
        zend_throw_exception(http_server_invalid_argument_exception_ce,
            "StaticHandler header name must not be empty", 0);
        return;
    }
    /* CRLF rejection is the load-bearing check (response splitting);
     * the rest of RFC 9110 §5.5 token charset is stricter but covered
     * by upstream parsers. */
    for (size_t i = 0; i < ZSTR_LEN(name); i++) {
        const unsigned char c = (unsigned char)ZSTR_VAL(name)[i];
        if (c < 0x20 || c == 0x7f || c == ':') {
            zend_throw_exception(http_server_invalid_argument_exception_ce,
                "StaticHandler header name contains an invalid character", 0);
            return;
        }
    }
    for (size_t i = 0; i < ZSTR_LEN(value); i++) {
        const unsigned char c = (unsigned char)ZSTR_VAL(value)[i];
        if (c == '\r' || c == '\n' || c == '\0') {
            zend_throw_exception(http_server_invalid_argument_exception_ce,
                "StaticHandler header value contains a control character", 0);
            return;
        }
    }

    if (mount->extra_headers == NULL) {
        ALLOC_HASHTABLE(mount->extra_headers);
        zend_hash_init(mount->extra_headers, 4, NULL, ZVAL_PTR_DTOR, 0);
    }

    zend_string *lower = zend_string_tolower(name);
    zval entry;
    ZVAL_STR(&entry, zend_string_copy(value));
    zend_hash_update(mount->extra_headers, lower, &entry);
    zend_string_release(lower);

    RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
}

ZEND_METHOD(TrueAsync_StaticHandler, setBrowseEnabled)
{
    bool enabled;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_BOOL(enabled)
    ZEND_PARSE_PARAMETERS_END();

    http_static_handler_t *mount = Z_HTTP_STATIC_HANDLER_P(ZEND_THIS);
    if (handler_check_locked(mount)) {
        return;
    }

    if (enabled) {
        mount->flags |= HTTP_STATIC_FLAG_BROWSE;
    } else {
        mount->flags &= ~HTTP_STATIC_FLAG_BROWSE;
    }

    RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
}

ZEND_METHOD(TrueAsync_StaticHandler, setMimeType)
{
    zend_string *extension = NULL;
    zend_string *content_type = NULL;

    ZEND_PARSE_PARAMETERS_START(2, 2)
        Z_PARAM_STR(extension)
        Z_PARAM_STR(content_type)
    ZEND_PARSE_PARAMETERS_END();

    http_static_handler_t *mount = Z_HTTP_STATIC_HANDLER_P(ZEND_THIS);
    if (handler_check_locked(mount)) {
        return;
    }

    if (ZSTR_LEN(extension) == 0) {
        zend_throw_exception(http_server_invalid_argument_exception_ce,
            "StaticHandler MIME extension must not be empty", 0);
        return;
    }
    if (ZSTR_VAL(extension)[0] == '.') {
        zend_throw_exception(http_server_invalid_argument_exception_ce,
            "StaticHandler MIME extension must not include the leading '.'", 0);
        return;
    }
    if (ZSTR_LEN(content_type) == 0) {
        zend_throw_exception(http_server_invalid_argument_exception_ce,
            "StaticHandler content type must not be empty", 0);
        return;
    }
    for (size_t i = 0; i < ZSTR_LEN(content_type); i++) {
        const unsigned char c = (unsigned char)ZSTR_VAL(content_type)[i];
        if (c == '\r' || c == '\n' || c == '\0') {
            zend_throw_exception(http_server_invalid_argument_exception_ce,
                "StaticHandler content type contains a control character", 0);
            return;
        }
    }

    if (mount->mime_overrides == NULL) {
        ALLOC_HASHTABLE(mount->mime_overrides);
        zend_hash_init(mount->mime_overrides, 4, NULL, ZVAL_PTR_DTOR, 0);
    }

    zend_string *lower_ext = zend_string_tolower(extension);
    zval entry;
    ZVAL_STR(&entry, zend_string_copy(content_type));
    zend_hash_update(mount->mime_overrides, lower_ext, &entry);
    zend_string_release(lower_ext);

    RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
}

ZEND_METHOD(TrueAsync_StaticHandler, getUrlPrefix)
{
    ZEND_PARSE_PARAMETERS_NONE();
    const http_static_handler_t *mount = Z_HTTP_STATIC_HANDLER_P(ZEND_THIS);
    if (mount->url_prefix == NULL) {
        RETURN_EMPTY_STRING();
    }
    RETURN_STR_COPY(mount->url_prefix);
}

ZEND_METHOD(TrueAsync_StaticHandler, getRootDirectory)
{
    ZEND_PARSE_PARAMETERS_NONE();
    const http_static_handler_t *mount = Z_HTTP_STATIC_HANDLER_P(ZEND_THIS);
    if (mount->root_directory == NULL) {
        RETURN_EMPTY_STRING();
    }
    RETURN_STR_COPY(mount->root_directory);
}

ZEND_METHOD(TrueAsync_StaticHandler, isLocked)
{
    ZEND_PARSE_PARAMETERS_NONE();
    const http_static_handler_t *mount = Z_HTTP_STATIC_HANDLER_P(ZEND_THIS);
    RETURN_BOOL((mount->flags & HTTP_STATIC_FLAG_LOCKED) != 0);
}

/* === Object lifecycle ================================================== */

static zend_object *http_static_handler_create(zend_class_entry *ce)
{
    http_static_handler_php_t *obj = zend_object_alloc(sizeof(*obj), ce);

    /* Zero out the descriptor — every field is a pointer/count/flag and
     * must start NULL/0. */
    memset(&obj->mount, 0, sizeof(obj->mount));

    zend_object_std_init(&obj->std, ce);
    object_properties_init(&obj->std, ce);
    obj->std.handlers = &http_static_handler_object_handlers;

    return &obj->std;
}

static void http_static_handler_free(zend_object *obj)
{
    http_static_handler_php_t *php_obj = http_static_handler_php_from_obj(obj);
    mount_release_owned(&php_obj->mount);
    zend_object_std_dtor(&php_obj->std);
}

void http_static_handler_class_register(void)
{
    http_static_handler_ce = register_class_TrueAsync_StaticHandler();
    http_static_handler_ce->create_object = http_static_handler_create;

    memcpy(&http_static_handler_object_handlers, &std_object_handlers,
           sizeof(zend_object_handlers));
    http_static_handler_object_handlers.offset =
        XtOffsetOf(http_static_handler_php_t, std);
    http_static_handler_object_handlers.free_obj = http_static_handler_free;
    http_static_handler_object_handlers.clone_obj = NULL;

    http_static_handler_ce->default_object_handlers =
        &http_static_handler_object_handlers;

    http_static_on_missing_ce = register_class_TrueAsync_StaticOnMissing();
    http_static_dotfiles_ce   = register_class_TrueAsync_StaticDotfiles();
    http_static_symlinks_ce   = register_class_TrueAsync_StaticSymlinks();
}
