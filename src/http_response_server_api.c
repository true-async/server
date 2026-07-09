/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

/* Server-side C-API for http_response_object — read accessors plus
 * setters used by protocol/static/compression code to populate a
 * response without going through the PHP setStatus/setHeader/setBody
 * guards. No closed/streaming gate here: the caller is the server. */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include "php.h"
#include "zend_smart_str.h"
#include "php_http_server.h"
#include "http1/http_parser.h"   /* http_request_t — for should_keep_alive */
#include "http_response_internal.h"

#ifndef PHP_WIN32
# include <strings.h>            /* strncasecmp */
#endif

/* ============================================================
 * Read accessors
 * ============================================================ */

void http_response_set_socket(zend_object *obj, php_socket_t fd)
{
    http_response_from_obj(obj)->socket_fd = fd;
}

void http_response_set_head(zend_object *obj, bool is_head)
{
    http_response_from_obj(obj)->is_head = is_head;
}

void http_response_set_protocol_version(zend_object *obj, const char *version)
{
    http_response_object *response = http_response_from_obj(obj);

    if (response->protocol_version) {
        zend_string_release(response->protocol_version);
    }

    response->protocol_version = zend_string_init(version, strlen(version), 0);
}

bool http_response_is_closed(zend_object *obj)
{
    return http_response_from_obj(obj)->closed;
}

bool http_response_is_committed(zend_object *obj)
{
    return http_response_from_obj(obj)->committed;
}

/* True once HttpResponse::send() has been called. Dispose paths use
 * this to skip the buffered-mode commit (headers are already on the
 * wire, the data provider drives the body via chunk_queue). */
bool http_response_is_streaming(zend_object *obj)
{
    return http_response_from_obj(obj)->streaming;
}

void http_response_set_committed(zend_object *obj)
{
    http_response_from_obj(obj)->committed = true;
}

int http_response_get_status(zend_object *obj)
{
    return http_response_from_obj(obj)->status_code;
}

HashTable *http_response_get_headers(zend_object *obj)
{
    return http_response_from_obj(obj)->headers;
}

HashTable *http_response_get_trailers(zend_object *obj)
{
    /* Returns NULL when no trailer was ever set — callers check
     * `zend_hash_num_elements(ht) > 0` after also null-guarding. */
    return http_response_from_obj(obj)->trailers;
}

const char *http_response_get_body(zend_object *obj, size_t *len_out)
{
    http_response_object *response = http_response_from_obj(obj);

    if (response->body_view != NULL) {
        if (len_out != NULL) { *len_out = ZSTR_LEN(response->body_view); }
        return ZSTR_VAL(response->body_view);
    }

    smart_str_0(&response->body);

    if (response->body.s == NULL) {
        if (len_out != NULL) { *len_out = 0; }
        return NULL;
    }

    if (len_out != NULL) { *len_out = ZSTR_LEN(response->body.s); }
    return ZSTR_VAL(response->body.s);
}

zend_string *http_response_get_body_str(zend_object *obj)
{
    http_response_object *response = http_response_from_obj(obj);

    if (response->body_view != NULL) {
        return response->body_view;
    }

    smart_str_0(&response->body);
    return response->body.s;  /* may be NULL */
}

/* Install streaming ops + ctx on the response. Protocol strategies
 * call this once at dispatch; reading after send() activates
 * streaming mode. Passing ops=NULL clears (not currently used). */
void http_response_install_stream_ops(zend_object *obj,
                                      const http_response_stream_ops_t *ops,
                                      void *ctx)
{
    http_response_object *response = http_response_from_obj(obj);
    response->stream_ops = ops;
    response->stream_ctx = ctx;
}

/* Force `Connection: close` into the response headers, overwriting any
 * value the handler set. Used by the graceful-drain path in
 * http_connection.c to signal "no more keep-alive on this TCP" — RFC
 * 9112 §9.6 explicitly permits servers to override the Connection
 * header, so this is safe regardless of what the handler wrote. */
void http_response_force_connection_close(zend_object *obj)
{
    http_response_object *response = http_response_from_obj(obj);

    zend_string *key = zend_string_init("connection", sizeof("connection") - 1, 0);

    zval value_zv, arr;
    ZVAL_STRINGL(&value_zv, "close", 5);
    array_init(&arr);
    add_next_index_zval(&arr, &value_zv);
    zend_hash_update(response->headers, key, &arr);

    zend_string_release(key);
}

/* Set the Alt-Svc header on the response if the handler has not
 * already set one. Server-driven advertisement of an HTTP/3 endpoint
 * — see RFC 7838 §3. The handler remains free to override (some apps
 * want per-request control over which alternative they advertise) —
 * that's why this is "if unset", not "force". */
void http_response_set_alt_svc_if_unset(zend_object *obj,
                                        const char *value, size_t valuelen)
{
    http_response_object *response = http_response_from_obj(obj);

    if (response->headers == NULL || value == NULL || valuelen == 0) {
        return;
    }

    zend_string *key =
        zend_string_init("alt-svc", sizeof("alt-svc") - 1, 0);

    if (zend_hash_exists(response->headers, key)) {
        zend_string_release(key);
        return;
    }

    zval value_zv, arr;
    ZVAL_STRINGL(&value_zv, value, valuelen);
    array_init(&arr);
    add_next_index_zval(&arr, &value_zv);
    zend_hash_update(response->headers, key, &arr);
    zend_string_release(key);
}

/* ============================================================
 * Static-handler internal API (issue #13).
 *
 * The dispatch path resolves the file in C without entering the PHP
 * VM, then populates status, headers, and body via these direct
 * field setters before the coroutine entry's skip_php_handler
 * short-circuit fires.
 * ============================================================ */

void http_response_static_set_status(zend_object *obj, int status_code)
{
    http_response_from_obj(obj)->status_code = status_code;
}

void http_response_static_set_header(zend_object *obj,
                                     const char *name, size_t name_len,
                                     const char *value, size_t value_len)
{
    http_response_object *r = http_response_from_obj(obj);
    /* Header names are stored lowercased — reader paths assume it. */
    zend_string *lower_name = zend_string_alloc(name_len, 0);
    for (size_t i = 0; i < name_len; i++) {
        char c = name[i];

        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        ZSTR_VAL(lower_name)[i] = c;
    }

    ZSTR_VAL(lower_name)[name_len] = '\0';

    zval header_value;
    ZVAL_STR(&header_value, zend_string_init(value, value_len, 0));
    zend_hash_update(r->headers, lower_name, &header_value);
    zend_string_release(lower_name);
}

/* Replace the buffered body with a copy of [data, data+len). Drops any
 * prior body_view; empty input leaves an empty body. */
static void response_set_body_bytes(http_response_object *r,
                                    const char *data, const size_t len)
{
    response_clear_body_view(r);
    smart_str_free(&r->body);

    if (data != NULL && len > 0) {
        smart_str_appendl(&r->body, data, len);
    }

    smart_str_0(&r->body);
}

void http_response_static_set_body_str(zend_object *obj, zend_string *body)
{
    http_response_object *r = http_response_from_obj(obj);

    if (body != NULL) {
        response_set_body_bytes(r, ZSTR_VAL(body), ZSTR_LEN(body));
    } else {
        response_set_body_bytes(r, NULL, 0);
    }
}

/* Zero-copy variant: keep a refcount on the caller's zend_string and
 * let the send-path emit it as a separate iov entry. The caller may
 * release its own ref immediately after — we hold our own bump. Body
 * must be non-empty (zero-length bodies use the smart_str path). */
void http_response_static_set_body_view(zend_object *obj, zend_string *body)
{
    http_response_object *r = http_response_from_obj(obj);
    response_clear_body_view(r);
    smart_str_free(&r->body);

    if (body != NULL && ZSTR_LEN(body) > 0) {
        zend_string_addref(body);
        r->body_view = body;
    }
}

/* C-string variant — skips the zend_string init/release dance the
 * static handler used to do for short literal bodies (404 "Not
 * Found", 500 "Internal Server Error", etc.). Behaviourally
 * identical to set_body_str: replaces any prior body. */
void http_response_static_set_body_cstr(zend_object *obj,
                                        const char *body, size_t body_len)
{
    http_response_object *r = http_response_from_obj(obj);
    response_set_body_bytes(r, body, body_len);
}

/* Internal "set canned error" — used when the dispatch layer rejects
 * a request before the handler runs (e.g. Content-Encoding decode
 * failure). Bypasses the PHP-facing setHeader/setStatusCode guards
 * because nothing has been committed yet. The dispose path emits it
 * exactly like a handler-built response.
 *
 * zend_hash_str_update avoids the per-call key zend_string allocation
 * a normal setHeader would perform; only the value is reified. */
void http_response_set_error(zend_object *obj, int status, const char *message)
{
    http_response_object *r = http_response_from_obj(obj);
    r->status_code = status;
    zval ct_z;
    ZVAL_STR(&ct_z, zend_string_init("text/plain; charset=utf-8", 25, 0));
    zend_hash_str_update(r->headers, "content-type", 12, &ct_z);
    response_clear_body_view(r);
    smart_str_free(&r->body);
    smart_str_appends(&r->body, message);
    smart_str_0(&r->body);
}

void http_response_reset_to_error(zend_object *obj, int status_code, const char *message)
{
    http_response_object *response = http_response_from_obj(obj);

    response->status_code = status_code;

    const size_t msg_len = strlen(message);
    response_set_body_bytes(response, message, msg_len);

    /* Clear custom headers */
    if (response->headers) {
        zend_hash_clean(response->headers);
    }

    if (response->reason_phrase) {
        zend_string_release(response->reason_phrase);
    }

    response->reason_phrase = zend_string_init(message, msg_len, 0);

    response->committed = true;
}

/* Manual digit-by-digit decimal format. ~15× faster than snprintf("%PRIu64")
 * on hot paths (no parse-format-string, no locale lookup). Buffer must be
 * at least 21 bytes (20 digits + NUL). Returns bytes written (excluding NUL). */
static inline size_t format_u64(char *out, uint64_t v)
{
    char tmp[20];
    size_t n = 0;
    do {
        tmp[n++] = (char)('0' + (v % 10));
        v /= 10;
    } while (v != 0);
    for (size_t i = 0; i < n; i++) {
        out[i] = tmp[n - 1 - i];
    }

    out[n] = '\0';
    return n;
}

void http_response_set_content_length(zend_object *obj, uint64_t length)
{
    char buf[24];
    const size_t n = format_u64(buf, length);
    http_response_static_set_header(obj, "content-length", 14, buf, n);
}

void http_response_apply_extra_headers(zend_object *obj, const HashTable *extra,
                                       const bool include_content_headers)
{
    if (extra == NULL) {
        return;
    }

    zend_string *name;
    zval        *value;
    ZEND_HASH_FOREACH_STR_KEY_VAL((HashTable *)extra, name, value) {
        if (name == NULL || Z_TYPE_P(value) != IS_STRING) {
            continue;
        }

        if (!include_content_headers && ZSTR_LEN(name) >= 8 &&
            strncasecmp(ZSTR_VAL(name), "content-", 8) == 0) {
            continue;
        }

        http_response_static_set_header(obj, ZSTR_VAL(name), ZSTR_LEN(name),
                                        Z_STRVAL_P(value), Z_STRLEN_P(value));
    } ZEND_HASH_FOREACH_END();
}

void http_response_set_connection(zend_object *obj, bool keep_alive)
{
    if (keep_alive) {
        http_response_static_set_header(obj, "connection", 10, "keep-alive", 10);
    } else {
        http_response_static_set_header(obj, "connection", 10, "close", 5);
    }
}

bool http_response_header_allowed_h2h3(const char *name, const size_t len)
{
    switch (len) {
    case 7:
        return zend_binary_strcasecmp(name, 7, "upgrade", 7) != 0;
    case 10:
        return zend_binary_strcasecmp(name, 10, "connection", 10) != 0 &&
               zend_binary_strcasecmp(name, 10, "keep-alive", 10) != 0;
    case 14:
        /* implicit from DATA frames */
        return zend_binary_strcasecmp(name, 14, "content-length", 14) != 0;
    case 17:
        return zend_binary_strcasecmp(name, 17, "transfer-encoding", 17) != 0;
    default:
        return true;
    }
}

bool http_response_should_keep_alive(const http_request_t *req)
{
    /* Parser populates req->keep_alive during on_headers_complete:
     * HTTP/1.1 defaults keep-alive unless "Connection: close",
     * HTTP/1.0 defaults close unless "Connection: keep-alive".
     * HTTP/2 / HTTP/3 streams: parser leaves it true; multiplex
     * transports filter Connection at submit time anyway. */
    return req != NULL && req->keep_alive;
}

void http_response_emit_status_body(zend_object *obj, int status_code,
                                    const char *body, size_t body_len)
{
    http_response_static_set_status(obj, status_code);
    http_response_static_set_header(obj, "content-type", 12,
                                    "text/plain; charset=utf-8", 25);

    if (body != NULL && body_len > 0) {
        http_response_static_set_body_cstr(obj, body, body_len);
    }
}

void http_response_synth_error(zend_object *obj, int status_code, const char *message)
{
    const size_t msg_len = (message != NULL) ? strlen(message) : 0;
    http_response_emit_status_body(obj, status_code, message, msg_len);
}
