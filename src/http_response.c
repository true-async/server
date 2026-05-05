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
#include "zend_smart_str.h"
#include "Zend/zend_async_API.h"     /* zend_async_event_t * for stream ops */
#include "ext/json/php_json.h"
#include "main/php_network.h"   /* php_socket_t, SOCK_ERR */
#include "php_http_server.h"
#include "smart_str_scalable.h"

/* Include generated arginfo */
#include "../stubs/HttpResponse.php_arginfo.h"

/* HTTP status reason phrases */
static const char *http_status_reason(int code)
{
    switch (code) {
        case 100: return "Continue";
        case 101: return "Switching Protocols";
        case 200: return "OK";
        case 201: return "Created";
        case 202: return "Accepted";
        case 204: return "No Content";
        case 206: return "Partial Content";
        case 301: return "Moved Permanently";
        case 302: return "Found";
        case 303: return "See Other";
        case 304: return "Not Modified";
        case 307: return "Temporary Redirect";
        case 308: return "Permanent Redirect";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 408: return "Request Timeout";
        case 409: return "Conflict";
        case 410: return "Gone";
        case 411: return "Length Required";
        case 413: return "Payload Too Large";
        case 414: return "URI Too Long";
        case 415: return "Unsupported Media Type";
        case 416: return "Range Not Satisfiable";
        case 422: return "Unprocessable Entity";
        case 429: return "Too Many Requests";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 502: return "Bad Gateway";
        case 503: return "Service Unavailable";
        case 504: return "Gateway Timeout";
        default:  return "Unknown";
    }
}

/* Response object structure.
 * Ordered by alignment: pointers & smart_str first, then socket_fd
 * (pointer-sized on Windows), then 32-bit status_code, then bool flags
 * clustered. zend_object must stay last for PHP object layout. */
typedef struct {
    zend_string     *reason_phrase;     /* Custom reason phrase (NULL = auto) */
    HashTable       *headers;           /* Response headers (name => array of values) */
    HashTable       *trailers;          /* HTTP/2 trailers (name => value zend_string); NULL until first setTrailer */
    zend_string     *protocol_version;  /* HTTP version (e.g., "1.1") */
    smart_str        body;              /* Body buffer (pointer + size_t) */

    /* Streaming ops + ctx. Installed by
     * the protocol strategy at dispatch; NULL for buffered-mode
     * responses. send() activates streaming by reading these; the
     * ops interpret ctx (opaque pointer to the protocol-specific
     * stream state). */
    const http_response_stream_ops_t *stream_ops;
    void                             *stream_ctx;

    /* Connection info (for sending). SOCK_ERR if not connected. */
    php_socket_t     socket_fd;

    /* HTTP status code */
    int              status_code;

    /* State flags (clustered) */
    bool             headers_sent;      /* Headers already sent? */
    bool             closed;            /* Response closed? */
    bool             committed;         /* Response fully prepared for sending? */
    bool             streaming;         /* send() has been called — setBody/setHeader now throw */

    /* Compression module state (issue #8). Opaque ptr — owned by the
     * compression TU; allocated by http_compression_attach at dispatch
     * and freed by http_compression_state_free at object dtor. NULL
     * when compression is disabled or the response was created
     * standalone (no dispatch). */
    void            *compression_state;

    zend_object      std;
} http_response_object;

/* Class entry */
zend_class_entry *http_response_ce;
static zend_object_handlers http_response_handlers;

/* Object retrieval macro */
static inline http_response_object *http_response_from_obj(zend_object *obj) {
    return (http_response_object *)((char *)(obj) - XtOffsetOf(http_response_object, std));
}
#define Z_HTTP_RESPONSE_P(zv) http_response_from_obj(Z_OBJ_P(zv))

/* Helper: gate every status/header/body mutation. A response is
 * no-longer-mutable in two states:
 *  1. closed   — end() has been called; nothing further is possible.
 *  2. streaming — send() has been called; status + headers are
 *                 committed on the wire. Trailers are still allowed
 *                 (they're post-DATA) and go through separate
 *                 non-guarded setters — see setTrailer/setTrailers. */
static inline bool response_check_closed(const http_response_object *response)
{
    if (response->closed) {
        zend_throw_exception(http_server_runtime_exception_ce,
            "Cannot modify response after end() has been called", 0);
        return true;
    }
    if (response->streaming) {
        zend_throw_exception(http_server_runtime_exception_ce,
            "Cannot modify response — headers already committed by send()", 0);
        return true;
    }
    return false;
}

/* Helper: Normalize header name to lowercase */
static zend_string *normalize_header_name(zend_string *name)
{
    zend_string *lower = zend_string_tolower(name);
    return lower;
}

/* Helper: Add value to header.
 *
 * Storage shape:
 *   - one value          → zval[IS_STRING]   (hot path: 1 hash slot, no
 *                                              nested ZEND_HASH_FOREACH at
 *                                              format time)
 *   - two or more values → zval[IS_ARRAY]    (preserves multi-value semantics
 *                                              on a second addHeader())
 * Readers (http_response_format / H2 / H3 / getHeader / getHeaderLine)
 * branch on Z_TYPE — see helper macros below. */
static void add_header_value(HashTable *headers, zend_string *name, zval *value, bool replace)
{
    zend_string *lower_name = normalize_header_name(name);
    zval *existing = zend_hash_find(headers, lower_name);

    if (replace || !existing) {
        if (Z_TYPE_P(value) == IS_ARRAY) {
            uint32_t count = zend_hash_num_elements(Z_ARRVAL_P(value));
            if (count == 1) {
                /* Single-element array — store the inner string directly. */
                zval *first = NULL;
                ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(value), first) {
                    break;
                } ZEND_HASH_FOREACH_END();
                zval copy;
                ZVAL_COPY(&copy, first);
                convert_to_string(&copy);
                zend_hash_update(headers, lower_name, &copy);
            } else {
                /* Multi-value: keep array shape. */
                zval arr;
                array_init(&arr);
                zval *val;
                ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(value), val) {
                    zval copy;
                    ZVAL_COPY(&copy, val);
                    convert_to_string(&copy);
                    add_next_index_zval(&arr, &copy);
                } ZEND_HASH_FOREACH_END();
                zend_hash_update(headers, lower_name, &arr);
            }
        } else {
            /* Single scalar — flat IS_STRING. */
            zval copy;
            ZVAL_COPY(&copy, value);
            convert_to_string(&copy);
            zend_hash_update(headers, lower_name, &copy);
        }
    } else {
        /* Append to existing — promote IS_STRING → IS_ARRAY on first
         * additional value. */
        if (Z_TYPE_P(existing) == IS_STRING) {
            zval arr;
            array_init(&arr);
            /* Move the existing string into the array (no extra refcount). */
            Z_TRY_ADDREF_P(existing);
            add_next_index_zval(&arr, existing);
            /* Drop original, install array. */
            zend_hash_update(headers, lower_name, &arr);
            existing = zend_hash_find(headers, lower_name);
        }
        if (Z_TYPE_P(value) == IS_ARRAY) {
            zval *val;
            ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(value), val) {
                zval copy;
                ZVAL_COPY(&copy, val);
                convert_to_string(&copy);
                add_next_index_zval(existing, &copy);
            } ZEND_HASH_FOREACH_END();
        } else {
            zval copy;
            ZVAL_COPY(&copy, value);
            convert_to_string(&copy);
            add_next_index_zval(existing, &copy);
        }
    }

    zend_string_release(lower_name);
}

/* {{{ proto private HttpResponse::__construct() */
ZEND_METHOD(TrueAsync_HttpResponse, __construct)
{
    /* This constructor is private - instances are created internally by server */
    ZEND_PARSE_PARAMETERS_NONE();
}
/* }}} */

/* {{{ proto HttpResponse::setStatusCode(int $code): static */
ZEND_METHOD(TrueAsync_HttpResponse, setStatusCode)
{
    zend_long code;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_LONG(code)
    ZEND_PARSE_PARAMETERS_END();

    http_response_object *response = Z_HTTP_RESPONSE_P(ZEND_THIS);

    if (response_check_closed(response)) {
        return;
    }

    if (code < 100 || code > 599) {
        zend_throw_exception(http_server_invalid_argument_exception_ce,
            "HTTP status code must be between 100 and 599", 0);
        return;
    }

    response->status_code = (int)code;

    RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
}
/* }}} */

/* {{{ proto HttpResponse::getStatusCode(): int */
ZEND_METHOD(TrueAsync_HttpResponse, getStatusCode)
{
    ZEND_PARSE_PARAMETERS_NONE();
    http_response_object *response = Z_HTTP_RESPONSE_P(ZEND_THIS);
    RETURN_LONG(response->status_code);
}
/* }}} */

/* {{{ proto HttpResponse::setReasonPhrase(string $phrase): static */
ZEND_METHOD(TrueAsync_HttpResponse, setReasonPhrase)
{
    zend_string *phrase;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_STR(phrase)
    ZEND_PARSE_PARAMETERS_END();

    http_response_object *response = Z_HTTP_RESPONSE_P(ZEND_THIS);

    if (response_check_closed(response)) {
        return;
    }

    if (response->reason_phrase) {
        zend_string_release(response->reason_phrase);
    }
    response->reason_phrase = zend_string_copy(phrase);

    RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
}
/* }}} */

/* {{{ proto HttpResponse::getReasonPhrase(): string */
ZEND_METHOD(TrueAsync_HttpResponse, getReasonPhrase)
{
    ZEND_PARSE_PARAMETERS_NONE();
    http_response_object *response = Z_HTTP_RESPONSE_P(ZEND_THIS);

    if (response->reason_phrase) {
        RETURN_STR_COPY(response->reason_phrase);
    }

    RETURN_STRING(http_status_reason(response->status_code));
}
/* }}} */

/* {{{ proto HttpResponse::setHeader(string $name, string|array $value): static */
ZEND_METHOD(TrueAsync_HttpResponse, setHeader)
{
    zend_string *name;
    zval *value;

    ZEND_PARSE_PARAMETERS_START(2, 2)
        Z_PARAM_STR(name)
        Z_PARAM_ZVAL(value)
    ZEND_PARSE_PARAMETERS_END();

    http_response_object *response = Z_HTTP_RESPONSE_P(ZEND_THIS);

    if (response_check_closed(response)) {
        return;
    }

    add_header_value(response->headers, name, value, true);

    RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
}
/* }}} */

/* {{{ proto HttpResponse::addHeader(string $name, string|array $value): static */
ZEND_METHOD(TrueAsync_HttpResponse, addHeader)
{
    zend_string *name;
    zval *value;

    ZEND_PARSE_PARAMETERS_START(2, 2)
        Z_PARAM_STR(name)
        Z_PARAM_ZVAL(value)
    ZEND_PARSE_PARAMETERS_END();

    http_response_object *response = Z_HTTP_RESPONSE_P(ZEND_THIS);

    if (response_check_closed(response)) {
        return;
    }

    add_header_value(response->headers, name, value, false);

    RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
}
/* }}} */

/* {{{ proto HttpResponse::hasHeader(string $name): bool */
ZEND_METHOD(TrueAsync_HttpResponse, hasHeader)
{
    zend_string *name;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_STR(name)
    ZEND_PARSE_PARAMETERS_END();

    http_response_object *response = Z_HTTP_RESPONSE_P(ZEND_THIS);

    zend_string *lower_name = normalize_header_name(name);
    bool exists = zend_hash_exists(response->headers, lower_name);
    zend_string_release(lower_name);

    RETURN_BOOL(exists);
}
/* }}} */

/* {{{ proto HttpResponse::getHeader(string $name): ?string */
ZEND_METHOD(TrueAsync_HttpResponse, getHeader)
{
    zend_string *name;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_STR(name)
    ZEND_PARSE_PARAMETERS_END();

    http_response_object *response = Z_HTTP_RESPONSE_P(ZEND_THIS);

    zend_string *lower_name = normalize_header_name(name);
    zval *values = zend_hash_find(response->headers, lower_name);
    zend_string_release(lower_name);

    if (!values) {
        RETURN_NULL();
    }
    if (Z_TYPE_P(values) == IS_STRING) {
        RETURN_STR_COPY(Z_STR_P(values));
    }
    if (Z_TYPE_P(values) == IS_ARRAY) {
        /* Return first value */
        zval *first = zend_hash_index_find(Z_ARRVAL_P(values), 0);
        if (first) {
            RETURN_ZVAL(first, 1, 0);
        }
    }
    RETURN_NULL();
}
/* }}} */

/* {{{ proto HttpResponse::getHeaderLine(string $name): string */
ZEND_METHOD(TrueAsync_HttpResponse, getHeaderLine)
{
    zend_string *name;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_STR(name)
    ZEND_PARSE_PARAMETERS_END();

    http_response_object *response = Z_HTTP_RESPONSE_P(ZEND_THIS);

    zend_string *lower_name = normalize_header_name(name);
    zval *values = zend_hash_find(response->headers, lower_name);
    zend_string_release(lower_name);

    if (!values) {
        RETURN_EMPTY_STRING();
    }
    if (Z_TYPE_P(values) == IS_STRING) {
        RETURN_STR_COPY(Z_STR_P(values));
    }
    if (Z_TYPE_P(values) != IS_ARRAY) {
        RETURN_EMPTY_STRING();
    }

    /* Join values with comma */
    smart_str result = {0};
    zval *val;
    bool first = true;

    ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(values), val) {
        if (!first) {
            smart_str_appends(&result, ", ");
        }
        smart_str_append(&result, Z_STR_P(val));
        first = false;
    } ZEND_HASH_FOREACH_END();

    smart_str_0(&result);

    if (result.s) {
        RETURN_STR(result.s);
    }
    RETURN_EMPTY_STRING();
}
/* }}} */

/* {{{ proto HttpResponse::getHeaders(): array */
ZEND_METHOD(TrueAsync_HttpResponse, getHeaders)
{
    ZEND_PARSE_PARAMETERS_NONE();

    http_response_object *response = Z_HTTP_RESPONSE_P(ZEND_THIS);

    /* Return copy of headers */
    RETURN_ARR(zend_array_dup(response->headers));
}
/* }}} */

/* {{{ proto HttpResponse::resetHeaders(): static */
ZEND_METHOD(TrueAsync_HttpResponse, resetHeaders)
{
    ZEND_PARSE_PARAMETERS_NONE();

    http_response_object *response = Z_HTTP_RESPONSE_P(ZEND_THIS);

    if (response_check_closed(response)) {
        return;
    }

    zend_hash_clean(response->headers);

    RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
}
/* }}} */

/* -------------------------------------------------------------------------
 * Trailers (HTTP/2 response footer).
 *
 * Stored as name => zend_string map. On HTTP/1 the trailers table is
 * simply ignored (chunked-encoding trailer emission can follow later
 * if a real use case lands). On HTTP/2 the strategy's
 * commit path iterates this table and emits a terminal HEADERS frame
 * via nghttp2_submit_trailer. Canonical gRPC consumer — `grpc-status`
 * and `grpc-message` live here.
 * ------------------------------------------------------------------------- */

static inline void ensure_trailers_table(http_response_object *response)
{
    if (response->trailers == NULL) {
        ALLOC_HASHTABLE(response->trailers);
        zend_hash_init(response->trailers, 4, NULL, ZVAL_PTR_DTOR, 0);
    }
}

/* {{{ proto HttpResponse::setTrailer(string $name, string $value): static */
ZEND_METHOD(TrueAsync_HttpResponse, setTrailer)
{
    zend_string *name;
    zend_string *value;

    ZEND_PARSE_PARAMETERS_START(2, 2)
        Z_PARAM_STR(name)
        Z_PARAM_STR(value)
    ZEND_PARSE_PARAMETERS_END();

    http_response_object *response = Z_HTTP_RESPONSE_P(ZEND_THIS);
    if (response_check_closed(response)) {
        return;
    }

    ensure_trailers_table(response);

    /* Lowercase the trailer name in-place (copy-and-transform) so the
     * HPACK encoder doesn't get uppercase from us — RFC 9113 §8.2.1
     * forbids uppercase header names over HTTP/2. */
    zend_string *lname = zend_string_tolower(name);
    zval v;
    ZVAL_STR_COPY(&v, value);
    zend_hash_update(response->trailers, lname, &v);
    zend_string_release(lname);

    RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
}
/* }}} */

/* {{{ proto HttpResponse::setTrailers(array $trailers): static */
ZEND_METHOD(TrueAsync_HttpResponse, setTrailers)
{
    HashTable *input;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_ARRAY_HT(input)
    ZEND_PARSE_PARAMETERS_END();

    http_response_object *response = Z_HTTP_RESPONSE_P(ZEND_THIS);
    if (response_check_closed(response)) {
        return;
    }

    ensure_trailers_table(response);

    zend_string *name;
    zval        *value_zv;
    ZEND_HASH_FOREACH_STR_KEY_VAL(input, name, value_zv) {
        if (name == NULL || Z_TYPE_P(value_zv) != IS_STRING) {
            continue;
        }
        zend_string *const lname = zend_string_tolower(name);
        zval v;
        ZVAL_STR_COPY(&v, Z_STR_P(value_zv));
        zend_hash_update(response->trailers, lname, &v);
        zend_string_release(lname);
    } ZEND_HASH_FOREACH_END();

    RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
}
/* }}} */

/* {{{ proto HttpResponse::resetTrailers(): static */
ZEND_METHOD(TrueAsync_HttpResponse, resetTrailers)
{
    ZEND_PARSE_PARAMETERS_NONE();

    http_response_object *response = Z_HTTP_RESPONSE_P(ZEND_THIS);
    if (response_check_closed(response)) {
        return;
    }
    if (response->trailers != NULL) {
        zend_hash_clean(response->trailers);
    }

    RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
}
/* }}} */

/* {{{ proto HttpResponse::getTrailers(): array */
ZEND_METHOD(TrueAsync_HttpResponse, getTrailers)
{
    ZEND_PARSE_PARAMETERS_NONE();

    http_response_object *response = Z_HTTP_RESPONSE_P(ZEND_THIS);
    if (response->trailers == NULL) {
        array_init(return_value);
        return;
    }
    ZVAL_ARR(return_value, zend_array_dup(response->trailers));
}
/* }}} */

/* {{{ proto HttpResponse::getProtocolName(): string */
ZEND_METHOD(TrueAsync_HttpResponse, getProtocolName)
{
    ZEND_PARSE_PARAMETERS_NONE();
    RETURN_STRING("HTTP");
}
/* }}} */

/* {{{ proto HttpResponse::getProtocolVersion(): string */
ZEND_METHOD(TrueAsync_HttpResponse, getProtocolVersion)
{
    ZEND_PARSE_PARAMETERS_NONE();
    http_response_object *response = Z_HTTP_RESPONSE_P(ZEND_THIS);

    if (response->protocol_version) {
        RETURN_STR_COPY(response->protocol_version);
    }
    RETURN_STRING("1.1");
}
/* }}} */

/* {{{ proto HttpResponse::write(string $data): static */
ZEND_METHOD(TrueAsync_HttpResponse, write)
{
    zend_string *data;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_STR(data)
    ZEND_PARSE_PARAMETERS_END();

    http_response_object *response = Z_HTTP_RESPONSE_P(ZEND_THIS);

    if (response_check_closed(response)) {
        return;
    }

    /* write() is the buffered-mode incremental API: handler calls it
     * N times with chunks and the full body goes out on end(). Size is
     * unknown up front — scalable-grow flips to doubling above 2 MiB
     * so a handler writing a 256 MiB body doesn't take 40 k mremap
     * calls. See smart_str_scalable.h. */
    http_smart_str_append_scalable(&response->body,
                                   ZSTR_VAL(data), ZSTR_LEN(data));

    RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
}
/* }}} */

/* {{{ proto HttpResponse::getBody(): string */
ZEND_METHOD(TrueAsync_HttpResponse, getBody)
{
    ZEND_PARSE_PARAMETERS_NONE();

    http_response_object *response = Z_HTTP_RESPONSE_P(ZEND_THIS);

    if (response->body.s) {
        /* Deep copy: smart_str_appendl/_free in subsequent write()/setBody
         * realloc/free the underlying buffer in place without honouring
         * external refcounts. A simple RETURN_STR_COPY (refcount bump)
         * would let those calls mutate / free memory that the caller
         * still holds — observable as "snapshot" strings updating
         * after the call (and a UAF on setBody if Zend MM reuses
         * the slot). */
        RETURN_STR(zend_string_dup(response->body.s, 0));
    }
    RETURN_EMPTY_STRING();
}
/* }}} */

/* {{{ proto HttpResponse::setBody(string $body): static */
ZEND_METHOD(TrueAsync_HttpResponse, setBody)
{
    zend_string *body;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_STR(body)
    ZEND_PARSE_PARAMETERS_END();

    http_response_object *response = Z_HTTP_RESPONSE_P(ZEND_THIS);

    if (response_check_closed(response)) {
        return;
    }

    /* Clear and set new body */
    smart_str_free(&response->body);
    smart_str_append(&response->body, body);

    RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
}
/* }}} */

/* {{{ proto HttpResponse::getBodyStream(): mixed */
ZEND_METHOD(TrueAsync_HttpResponse, getBodyStream)
{
    ZEND_PARSE_PARAMETERS_NONE();

    /* TODO: Implement body stream support */
    RETURN_NULL();
}
/* }}} */

/* {{{ proto HttpResponse::setBodyStream(mixed $stream): static */
ZEND_METHOD(TrueAsync_HttpResponse, setBodyStream)
{
    zval *stream;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_ZVAL(stream)
    ZEND_PARSE_PARAMETERS_END();

    /* TODO: Implement body stream support */
    zend_throw_exception(http_server_runtime_exception_ce,
        "Body stream support is not yet implemented", 0);
}
/* }}} */

/* {{{ proto HttpResponse::json(mixed $data): static */
ZEND_METHOD(TrueAsync_HttpResponse, json)
{
    zval *data;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_ZVAL(data)
    ZEND_PARSE_PARAMETERS_END();

    http_response_object *response = Z_HTTP_RESPONSE_P(ZEND_THIS);

    if (response_check_closed(response)) {
        return;
    }

    /* Set Content-Type header */
    zval content_type;
    ZVAL_STRING(&content_type, "application/json");
    zend_string *ct_name = zend_string_init("content-type", sizeof("content-type") - 1, 0);
    add_header_value(response->headers, ct_name, &content_type, true);
    zend_string_release(ct_name);
    zval_ptr_dtor(&content_type);

    /* Encode JSON */
    smart_str json = {0};
    php_json_encode(&json, data, PHP_JSON_UNESCAPED_UNICODE);
    smart_str_0(&json);

    if (json.s) {
        smart_str_free(&response->body);
        response->body = json;
    }

    RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
}
/* }}} */

/* {{{ proto HttpResponse::html(string $html): static */
ZEND_METHOD(TrueAsync_HttpResponse, html)
{
    zend_string *html;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_STR(html)
    ZEND_PARSE_PARAMETERS_END();

    http_response_object *response = Z_HTTP_RESPONSE_P(ZEND_THIS);

    if (response_check_closed(response)) {
        return;
    }

    /* Set Content-Type header */
    zval content_type;
    ZVAL_STRING(&content_type, "text/html; charset=utf-8");
    zend_string *ct_name = zend_string_init("content-type", sizeof("content-type") - 1, 0);
    add_header_value(response->headers, ct_name, &content_type, true);
    zend_string_release(ct_name);
    zval_ptr_dtor(&content_type);

    /* Set body */
    smart_str_free(&response->body);
    smart_str_append(&response->body, html);

    RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
}
/* }}} */

/* {{{ proto HttpResponse::redirect(string $url, int $status = 302): static */
ZEND_METHOD(TrueAsync_HttpResponse, redirect)
{
    zend_string *url;
    zend_long status = 302;

    ZEND_PARSE_PARAMETERS_START(1, 2)
        Z_PARAM_STR(url)
        Z_PARAM_OPTIONAL
        Z_PARAM_LONG(status)
    ZEND_PARSE_PARAMETERS_END();

    http_response_object *response = Z_HTTP_RESPONSE_P(ZEND_THIS);

    if (response_check_closed(response)) {
        return;
    }

    /* Validate redirect status */
    if (status < 300 || status > 399) {
        zend_throw_exception(http_server_invalid_argument_exception_ce,
            "Redirect status must be between 300 and 399", 0);
        return;
    }

    response->status_code = (int)status;

    /* Set Location header */
    zval location;
    ZVAL_STR_COPY(&location, url);
    zend_string *header_name = zend_string_init("location", sizeof("location") - 1, 0);
    add_header_value(response->headers, header_name, &location, true);
    zend_string_release(header_name);
    zval_ptr_dtor(&location);

    RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
}
/* }}} */

/* {{{ proto HttpResponse::send(string $chunk): static
 *
 * Streaming response — append a chunk to the outbound queue. First
 * call commits status + headers (they can no longer be changed).
 * Subsequent calls append DATA frames (HTTP/2) / chunked-transfer
 * segments (HTTP/1). Blocks the handler coroutine only when the
 * per-stream queue crosses server->stream_write_buffer_bytes
 * (default 256 KiB); otherwise returns immediately.
 *
 * Throws when called on a response that has no stream ops installed
 * (typically a response detached from a real connection). */
ZEND_METHOD(TrueAsync_HttpResponse, send)
{
    zend_string *chunk;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_STR(chunk)
    ZEND_PARSE_PARAMETERS_END();

    http_response_object *const response = Z_HTTP_RESPONSE_P(ZEND_THIS);

    if (response->closed) {
        zend_throw_exception(http_server_runtime_exception_ce,
            "Response already closed — cannot send() after end()", 0);
        return;
    }
    if (response->stream_ops == NULL) {
        /* No stream ops installed — response is detached from a
         * connection (e.g. constructed standalone in user code). */
        zend_throw_exception(http_server_runtime_exception_ce,
            "Response streaming (send()) is not available on this response", 0);
        return;
    }

    /* First send() — lock headers and switch to streaming mode.
     * After this, setBody / setHeader / setStatusCode throw. */
    if (!response->streaming) {
        response->streaming = true;
        response->committed = true;
        response->headers_sent = true;
#ifdef HAVE_HTTP_COMPRESSION
        /* Wrap stream_ops with a compressing one if Accept-Encoding +
         * response state allow gzip. Mutates Content-Encoding/Vary on
         * the response so the stream's underlying header-commit picks
         * them up on the next line. */
        {
            extern void http_compression_maybe_install_stream_wrapper(zend_object *);
            http_compression_maybe_install_stream_wrapper(Z_OBJ_P(ZEND_THIS));
        }
#endif
    }

    /* Hand ownership of the chunk to the queue — the ops layer
     * takes a refcount. Empty chunks are still accepted (some
     * protocols use them as keepalive signals). */
    zend_string_addref(chunk);
    const int rc = response->stream_ops->append_chunk(
        response->stream_ctx, chunk);

    if (rc == HTTP_STREAM_APPEND_STREAM_DEAD) {
        /* Peer aborted between dispatch and now. Emulate the
         * cancel path — handler may catch and wind down gracefully. */
        zend_throw_exception_ex(http_exception_ce, 499,
            "stream closed by peer");
        return;
    }

    /* Backpressure handled inside append_chunk for protocols that
     * support per-stream flow control (H2/H3); H1 returns OK
     * unconditionally and relies on kernel send-buffer pushback. */
    (void)rc;

    RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
}
/* }}} */

/* {{{ proto HttpResponse::end(?string $data = null): void */
ZEND_METHOD(TrueAsync_HttpResponse, end)
{
    zend_string *data = NULL;

    ZEND_PARSE_PARAMETERS_START(0, 1)
        Z_PARAM_OPTIONAL
        Z_PARAM_STR_OR_NULL(data)
    ZEND_PARSE_PARAMETERS_END();

    http_response_object *response = Z_HTTP_RESPONSE_P(ZEND_THIS);

    if (response->closed) {
        zend_throw_exception(http_server_runtime_exception_ce,
            "Response has already been closed", 0);
        return;
    }

    /* Streaming path — optional final data goes as one last chunk,
     * then mark_ended drives the data provider to emit EOF. */
    if (response->streaming) {
        if (data != NULL && ZSTR_LEN(data) > 0) {
            zend_string_addref(data);
            (void)response->stream_ops->append_chunk(
                response->stream_ctx, data);
        }
        response->stream_ops->mark_ended(response->stream_ctx);
        response->closed = true;
        return;
    }

    /* Append final data if provided */
    if (data) {
        smart_str_append(&response->body, data);
    }

    /* Mark as closed */
    response->closed = true;
    response->headers_sent = true;

    /* Note: Actual sending to socket happens in connection handler,
     * which will call http_response_format() to get the raw response */
}
/* }}} */

/* {{{ proto HttpResponse::setNoCompression(): static
 *
 * Mark the response as ineligible for compression — overrides every
 * other rule. Use on responses that mix secrets with reflected user
 * input (BREACH mitigation), pre-compressed payloads, or anything the
 * server should not wrap in Content-Encoding. Idempotent. */
ZEND_METHOD(TrueAsync_HttpResponse, setNoCompression)
{
    ZEND_PARSE_PARAMETERS_NONE();
#ifdef HAVE_HTTP_COMPRESSION
    extern void http_compression_mark_no_compression(zend_object *obj);
    http_compression_mark_no_compression(Z_OBJ_P(ZEND_THIS));
#endif
    RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
}
/* }}} */

/* {{{ proto HttpResponse::isHeadersSent(): bool */
ZEND_METHOD(TrueAsync_HttpResponse, isHeadersSent)
{
    ZEND_PARSE_PARAMETERS_NONE();
    http_response_object *response = Z_HTTP_RESPONSE_P(ZEND_THIS);
    RETURN_BOOL(response->headers_sent);
}
/* }}} */

/* {{{ proto HttpResponse::isClosed(): bool */
ZEND_METHOD(TrueAsync_HttpResponse, isClosed)
{
    ZEND_PARSE_PARAMETERS_NONE();
    http_response_object *response = Z_HTTP_RESPONSE_P(ZEND_THIS);
    RETURN_BOOL(response->closed);
}
/* }}} */

/* Object handlers */

static zend_object *http_response_create(zend_class_entry *ce)
{
    http_response_object *response = zend_object_alloc(sizeof(http_response_object), ce);

    response->status_code = 200;
    response->reason_phrase = NULL;
    response->protocol_version = NULL;
    response->headers_sent = false;
    response->closed = false;
    response->committed = false;
    response->streaming = false;
    response->stream_ops = NULL;
    response->stream_ctx = NULL;
    response->compression_state = NULL;
    response->socket_fd = SOCK_ERR;
    memset(&response->body, 0, sizeof(smart_str));

    /* Initialize headers hash table */
    ALLOC_HASHTABLE(response->headers);
    zend_hash_init(response->headers, 8, NULL, ZVAL_PTR_DTOR, 0);

    /* Trailers table is lazy — most responses never set one. */
    response->trailers = NULL;

    zend_object_std_init(&response->std, ce);
    object_properties_init(&response->std, ce);
    response->std.handlers = &http_response_handlers;

    return &response->std;
}

static void http_response_free(zend_object *obj)
{
    http_response_object *response = http_response_from_obj(obj);

    if (response->reason_phrase) {
        zend_string_release(response->reason_phrase);
    }

    if (response->protocol_version) {
        zend_string_release(response->protocol_version);
    }

    if (response->headers) {
        zend_hash_destroy(response->headers);
        FREE_HASHTABLE(response->headers);
    }

    /* Trailers table is created lazily on first setTrailer — NULL-safe. */
    if (response->trailers) {
        zend_hash_destroy(response->trailers);
        FREE_HASHTABLE(response->trailers);
    }

    smart_str_free(&response->body);

#ifdef HAVE_HTTP_COMPRESSION
    /* Compression state is owned by the compression TU; reach in only
     * via the dedicated free helper — keeps the response struct opaque
     * to that side. NULL-safe. */
    {
        extern void http_compression_state_free(zend_object *obj);
        http_compression_state_free(obj);
    }
#endif

    zend_object_std_dtor(&response->std);
}

/* ============================================================
 * Accessors used by the compression module (issue #8). Kept here
 * so http_response_object stays private to this TU.
 * ============================================================ */

void *http_response_get_compression_slot(zend_object *obj)
{
    return http_response_from_obj(obj)->compression_state;
}

void http_response_set_compression_slot(zend_object *obj, void *p)
{
    http_response_from_obj(obj)->compression_state = p;
}

const http_response_stream_ops_t *http_response_get_stream_ops(zend_object *obj)
{
    return http_response_from_obj(obj)->stream_ops;
}

void *http_response_get_stream_ctx(zend_object *obj)
{
    return http_response_from_obj(obj)->stream_ctx;
}

void http_response_replace_stream_ops(zend_object *obj,
                                      const http_response_stream_ops_t *ops,
                                      void *ctx)
{
    http_response_object *r = http_response_from_obj(obj);
    r->stream_ops = ops;
    r->stream_ctx = ctx;
}

smart_str *http_response_get_body_smart_str(zend_object *obj)
{
    return &http_response_from_obj(obj)->body;
}

/* {{{ http_response_class_register */
void http_response_class_register(void)
{
    http_response_ce = register_class_TrueAsync_HttpResponse();
    http_response_ce->create_object = http_response_create;

    memcpy(&http_response_handlers, &std_object_handlers, sizeof(zend_object_handlers));
    http_response_handlers.offset = XtOffsetOf(http_response_object, std);
    http_response_handlers.free_obj = http_response_free;
    http_response_handlers.clone_obj = NULL;
}
/* }}} */

/* Internal API for connection handler */

/* Pre-baked status line table for common responses on HTTP/1.1.
 * Replaces the seven-append smart_str dance (HTTP/1.1 + code + reason
 * + CRLF) with a single smart_str_appendl of a constant. Hit-rate is
 * high — nearly every REST response is 200/201/204/301/400/404/500.
 * Codes outside this table fall through to the runtime builder, which
 * uses the handler-supplied reason phrase when present. */
typedef struct { const char *line; size_t len; } http_status_line_t;
#define MK_LINE(s) { s, sizeof(s) - 1 }
static const http_status_line_t http11_status_lines[] = {
    [200] = MK_LINE("HTTP/1.1 200 OK\r\n"),
    [201] = MK_LINE("HTTP/1.1 201 Created\r\n"),
    [202] = MK_LINE("HTTP/1.1 202 Accepted\r\n"),
    [204] = MK_LINE("HTTP/1.1 204 No Content\r\n"),
    [206] = MK_LINE("HTTP/1.1 206 Partial Content\r\n"),
    [301] = MK_LINE("HTTP/1.1 301 Moved Permanently\r\n"),
    [302] = MK_LINE("HTTP/1.1 302 Found\r\n"),
    [304] = MK_LINE("HTTP/1.1 304 Not Modified\r\n"),
    [400] = MK_LINE("HTTP/1.1 400 Bad Request\r\n"),
    [401] = MK_LINE("HTTP/1.1 401 Unauthorized\r\n"),
    [403] = MK_LINE("HTTP/1.1 403 Forbidden\r\n"),
    [404] = MK_LINE("HTTP/1.1 404 Not Found\r\n"),
    [405] = MK_LINE("HTTP/1.1 405 Method Not Allowed\r\n"),
    [409] = MK_LINE("HTTP/1.1 409 Conflict\r\n"),
    [413] = MK_LINE("HTTP/1.1 413 Payload Too Large\r\n"),
    [414] = MK_LINE("HTTP/1.1 414 URI Too Long\r\n"),
    [429] = MK_LINE("HTTP/1.1 429 Too Many Requests\r\n"),
    [500] = MK_LINE("HTTP/1.1 500 Internal Server Error\r\n"),
    [502] = MK_LINE("HTTP/1.1 502 Bad Gateway\r\n"),
    [503] = MK_LINE("HTTP/1.1 503 Service Unavailable\r\n"),
    [504] = MK_LINE("HTTP/1.1 504 Gateway Timeout\r\n"),
};
#undef MK_LINE
#define HTTP11_STATUS_LINES_CNT \
    (sizeof(http11_status_lines) / sizeof(http11_status_lines[0]))

/* Emit status line into result. Fast path: HTTP/1.1 + no custom reason
 * phrase + code in the pre-baked table → single memcpy. Everything
 * else falls back to the piecewise builder. */
static inline void emit_status_line(smart_str *result,
                                    const http_response_object *response)
{
    const int code = response->status_code;
    const bool is_http11 = response->protocol_version == NULL
        || (ZSTR_LEN(response->protocol_version) == 3
            && memcmp(ZSTR_VAL(response->protocol_version), "1.1", 3) == 0);

    if (is_http11 && response->reason_phrase == NULL
        && code > 0 && (size_t)code < HTTP11_STATUS_LINES_CNT
        && http11_status_lines[code].line != NULL) {
        smart_str_appendl(result, http11_status_lines[code].line,
                          http11_status_lines[code].len);
        return;
    }

    const char *version = response->protocol_version
                              ? ZSTR_VAL(response->protocol_version) : "1.1";
    const char *reason  = response->reason_phrase
                              ? ZSTR_VAL(response->reason_phrase)
                              : http_status_reason(code);
    smart_str_appends(result, "HTTP/");
    smart_str_appends(result, version);
    smart_str_appendc(result, ' ');
    smart_str_append_long(result, code);
    smart_str_appendc(result, ' ');
    smart_str_appends(result, reason);
    smart_str_appends(result, "\r\n");
}

/* Internal: append status line + Content-Length + headers + CRLF terminator
 * into @p result. Body is NOT appended — callers either append it themselves
 * (legacy http_response_format) or send it as a separate iov entry
 * (http_response_format_parts). Splitting this out keeps the two formatters
 * byte-identical for the headers section. */
static void emit_headers_block(smart_str *result, http_response_object *response,
                               size_t body_len)
{
    emit_status_line(result, response);

    /* Add Content-Length if body exists and not already set. Use
     * zend_hash_str_exists to skip the zend_string alloc/release
     * round-trip on the literal name lookup. */
    if (!zend_hash_str_exists(response->headers, "content-length",
                              sizeof("content-length") - 1)) {
        smart_str_appendl(result, "Content-Length: ", sizeof("Content-Length: ") - 1);
        smart_str_append_unsigned(result, body_len);
        smart_str_appendl(result, "\r\n", 2);
    }

    /* Headers — flat IS_STRING avoids nested foreach for the
     * single-value common case (§4.4 perf). */
    zend_string *name;
    zval *values;
    ZEND_HASH_FOREACH_STR_KEY_VAL(response->headers, name, values) {
        if (UNEXPECTED(name == NULL)) continue;
        if (EXPECTED(Z_TYPE_P(values) == IS_STRING)) {
            smart_str_append(result, name);
            smart_str_appends(result, ": ");
            smart_str_append(result, Z_STR_P(values));
            smart_str_appends(result, "\r\n");
        } else if (Z_TYPE_P(values) == IS_ARRAY) {
            zval *val;
            ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(values), val) {
                smart_str_append(result, name);
                smart_str_appends(result, ": ");
                smart_str_append(result, Z_STR_P(val));
                smart_str_appends(result, "\r\n");
            } ZEND_HASH_FOREACH_END();
        }
    } ZEND_HASH_FOREACH_END();

    /* End of headers */
    smart_str_appends(result, "\r\n");
}

/* Body length for the threshold branch in the dispose hot path. Reads
 * the smart_str's terminator-safe length without exposing the response
 * struct. Returns 0 when no body has been buffered. */
size_t http_response_get_body_len(zend_object *obj)
{
    http_response_object *response = http_response_from_obj(obj);
    return response->body.s != NULL ? ZSTR_LEN(response->body.s) : 0;
}

/* Vectored format: produces the headers block as one zend_string and hands
 * back the body as a refcount-bumped reference (or NULL if empty). Caller
 * owns both refs and must release them — typically by passing the pair to
 * ZEND_ASYNC_IO_WRITEV, where the reactor consumes the refs on completion.
 *
 * Saves one emalloc + one memcpy per response compared to http_response_format
 * (which concatenates headers + body into a single zend_string). */
void http_response_format_parts(zend_object *obj,
                                zend_string **headers_out,
                                zend_string **body_out)
{
    http_response_object *response = http_response_from_obj(obj);
    smart_str result = {0};

#ifdef HAVE_HTTP_COMPRESSION
    {
        extern void http_compression_apply_buffered(zend_object *);
        http_compression_apply_buffered(obj);
    }
#endif

    smart_str_0(&response->body);
    const size_t body_len = response->body.s ? ZSTR_LEN(response->body.s) : 0;

    emit_headers_block(&result, response, body_len);
    smart_str_0(&result);

    *headers_out = result.s ? result.s : zend_empty_string;
    if (response->body.s != NULL && body_len > 0) {
        zend_string_addref(response->body.s);
        *body_out = response->body.s;
    } else {
        *body_out = NULL;
    }
}

/* Format response as a single HTTP string (legacy: headers + body
 * concatenated). Hot-path callers prefer http_response_format_parts +
 * ZEND_ASYNC_IO_WRITEV which skips the body memcpy; this stays for the
 * TLS path and assorted error/HEAD/204 callers that already work in
 * single-buffer terms. */
zend_string *http_response_format(zend_object *obj)
{
    http_response_object *response = http_response_from_obj(obj);
    smart_str result = {0};

#ifdef HAVE_HTTP_COMPRESSION
    {
        extern void http_compression_apply_buffered(zend_object *);
        http_compression_apply_buffered(obj);
    }
#endif

    smart_str_0(&response->body);
    const size_t body_len = response->body.s ? ZSTR_LEN(response->body.s) : 0;

    emit_headers_block(&result, response, body_len);

    if (response->body.s && body_len > 0) {
        smart_str_append(&result, response->body.s);
    }

    smart_str_0(&result);

    return result.s ? result.s : zend_empty_string;
}

/* Serialize status line + headers for an HTTP/1.1 streaming response
 * (Transfer-Encoding: chunked). Strips any handler-set Content-Length
 * — incompatible with chunked per RFC 9112 §6.3 — and emits
 * `Transfer-Encoding: chunked` in its place. Headers end with the
 * empty line; the caller writes the body as a sequence of chunks.
 *
 * Used by h1_stream_ops at first send(). Separate from http_response_format
 * because the latter builds status + Content-Length + headers + body
 * as a single atomic payload, which is exactly what chunked avoids. */
zend_string *http_response_format_streaming_headers(zend_object *obj)
{
    http_response_object *response = http_response_from_obj(obj);
    smart_str result = {0};

    emit_status_line(&result, response);
    smart_str_appends(&result, "Transfer-Encoding: chunked\r\n");

    zend_string *name;
    zval        *values;
    ZEND_HASH_FOREACH_STR_KEY_VAL(response->headers, name, values) {
        if (UNEXPECTED(name == NULL)) continue;
        /* Skip Content-Length: incompatible with chunked. Case-
         * insensitive match since handlers may set it either way. */
        if (ZSTR_LEN(name) == 14 &&
            zend_binary_strcasecmp(ZSTR_VAL(name), 14, "content-length", 14) == 0) {
            continue;
        }
        /* Skip any Transfer-Encoding the handler pre-set (we always
         * set chunked ourselves; a conflicting value would confuse
         * intermediaries). */
        if (ZSTR_LEN(name) == 17 &&
            zend_binary_strcasecmp(ZSTR_VAL(name), 17, "transfer-encoding", 17) == 0) {
            continue;
        }
        if (EXPECTED(Z_TYPE_P(values) == IS_STRING)) {
            smart_str_append(&result, name);
            smart_str_appends(&result, ": ");
            smart_str_append(&result, Z_STR_P(values));
            smart_str_appends(&result, "\r\n");
        } else if (Z_TYPE_P(values) == IS_ARRAY) {
            zval *val;
            ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(values), val) {
                if (Z_TYPE_P(val) != IS_STRING) { continue; }
                smart_str_append(&result, name);
                smart_str_appends(&result, ": ");
                smart_str_append(&result, Z_STR_P(val));
                smart_str_appends(&result, "\r\n");
            } ZEND_HASH_FOREACH_END();
        }
    } ZEND_HASH_FOREACH_END();

    smart_str_appends(&result, "\r\n");
    smart_str_0(&result);
    return result.s ? result.s : zend_empty_string;
}

/* Set socket fd for response */
void http_response_set_socket(zend_object *obj, php_socket_t fd)
{
    http_response_object *response = http_response_from_obj(obj);
    response->socket_fd = fd;
}

/* Set protocol version */
void http_response_set_protocol_version(zend_object *obj, const char *version)
{
    http_response_object *response = http_response_from_obj(obj);
    if (response->protocol_version) {
        zend_string_release(response->protocol_version);
    }
    response->protocol_version = zend_string_init(version, strlen(version), 0);
}

/* Check if response is closed */
bool http_response_is_closed(zend_object *obj)
{
    http_response_object *response = http_response_from_obj(obj);
    return response->closed;
}

bool http_response_is_committed(zend_object *obj)
{
    http_response_object *response = http_response_from_obj(obj);
    return response->committed;
}

/* True once HttpResponse::send() has been called. Dispose paths use
 * this to skip the buffered-mode commit (headers are already on the
 * wire, the data provider drives the body via chunk_queue). */
bool http_response_is_streaming(zend_object *obj)
{
    http_response_object *response = http_response_from_obj(obj);
    return response->streaming;
}

void http_response_set_committed(zend_object *obj)
{
    http_response_object *response = http_response_from_obj(obj);
    response->committed = true;
}

/* Accessors used by the HTTP/2 strategy to serialise responses
 * without going through the HTTP/1 text formatter. Keep the internal
 * http_response_object struct opaque to the rest of the tree. */
int http_response_get_status(zend_object *obj)
{
    return http_response_from_obj(obj)->status_code;
}

HashTable *http_response_get_headers(zend_object *obj)
{
    return http_response_from_obj(obj)->headers;
}

/* Install streaming ops + ctx on the response. Protocol strategies
 * call this once at dispatch; reading after send() activates
 * streaming mode. Passing ops=NULL clears (not currently used). */
void http_response_install_stream_ops(zend_object *obj,
                                      const http_response_stream_ops_t *ops,
                                      void *ctx)
{
    http_response_object *const response = http_response_from_obj(obj);
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
    http_response_object *const response = http_response_from_obj(obj);

    zend_string *const key = zend_string_init("connection", sizeof("connection") - 1, 0);

    zval value_zv, arr;
    ZVAL_STRINGL(&value_zv, "close", 5);
    array_init(&arr);
    add_next_index_zval(&arr, &value_zv);
    zend_hash_update(response->headers, key, &arr);

    zend_string_release(key);
}

/* Set the Alt-Svc header on the response if the handler has not
 * already set one. Server-driven advertisement of an HTTP/3 endpoint
 * — see RFC 7838 §3. The handler
 * remains free to override (some apps want per-request control over
 * which alternative they advertise) — that's why this is "if unset",
 * not "force". */
void http_response_set_alt_svc_if_unset(zend_object *obj,
                                        const char *value, size_t valuelen)
{
    http_response_object *const response = http_response_from_obj(obj);
    if (response->headers == NULL || value == NULL || valuelen == 0) {
        return;
    }
    zend_string *const key =
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

HashTable *http_response_get_trailers(zend_object *obj)
{
    /* Returns NULL when no trailer was ever set — callers check
     * `zend_hash_num_elements(ht) > 0` after also null-guarding. */
    return http_response_from_obj(obj)->trailers;
}

const char *http_response_get_body(zend_object *obj, size_t *len_out)
{
    http_response_object *const response = http_response_from_obj(obj);
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
    http_response_object *const response = http_response_from_obj(obj);
    smart_str_0(&response->body);
    return response->body.s;  /* may be NULL */
}

void http_response_reset_to_error(zend_object *obj, int status_code, const char *message)
{
    http_response_object *response = http_response_from_obj(obj);

    /* Reset status */
    response->status_code = status_code;

    /* Reset body */
    smart_str_free(&response->body);
    smart_str_appends(&response->body, message);
    smart_str_0(&response->body);

    /* Clear custom headers */
    if (response->headers) {
        zend_hash_clean(response->headers);
    }

    /* Set reason phrase */
    if (response->reason_phrase) {
        zend_string_release(response->reason_phrase);
    }
    response->reason_phrase = zend_string_init(message, strlen(message), 0);

    response->committed = true;
}

