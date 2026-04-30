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

/* -------------------------------------------------------------------------
 * Server-Sent Events (text/event-stream).
 *
 * SSE is not a separate protocol — it's a Content-Type convention plus
 * a small text format on top of the existing chunked / DATA-framed
 * streaming pipeline. These helpers exist to (1) ensure the canonical
 * three headers are set so handlers don't ship a broken stream behind
 * nginx/CDN, and (2) format event records correctly per WHATWG §9.2 so
 * handlers don't reinvent the framing.
 *
 * Wire commit is lazy: sseStart() sets headers and locks the response
 * into streaming mode at the PHP boundary, but the actual HEADERS frame
 * (H2/H3) or status line (H1 chunked) is emitted by the stream_ops on
 * the first append_chunk. If the handler only calls sseStart() and then
 * end(), the mark_ended path flushes headers + EOF. ------------------------------------------------------------------------- */

/* SSE field separators: events are delimited by a blank line, fields by
 * a single LF. WHATWG mandates LF; CR / CRLF are accepted on input but
 * we always emit LF to keep the parser fast-path happy. */

static inline bool sse_string_has_newline(zend_string *s)
{
    if (s == NULL) {
        return false;
    }
    const char *const p = ZSTR_VAL(s);
    const size_t len = ZSTR_LEN(s);
    for (size_t i = 0; i < len; i++) {
        if (p[i] == '\n' || p[i] == '\r') {
            return true;
        }
    }
    return false;
}

/* If the handler already set Content-Type to anything other than
 * text/event-stream, throw — sseStart() is an explicit switch and a
 * conflicting type is a programming bug, not something to paper over. */
static bool sse_validate_content_type(http_response_object *response)
{
    zval *const ct = zend_hash_str_find(
        response->headers, "content-type", sizeof("content-type") - 1);
    if (ct == NULL) {
        return true;
    }
    const char *val = NULL;
    size_t      val_len = 0;
    if (Z_TYPE_P(ct) == IS_STRING) {
        val     = Z_STRVAL_P(ct);
        val_len = Z_STRLEN_P(ct);
    } else if (Z_TYPE_P(ct) == IS_ARRAY) {
        zval *const first = zend_hash_index_find(Z_ARRVAL_P(ct), 0);
        if (first != NULL && Z_TYPE_P(first) == IS_STRING) {
            val     = Z_STRVAL_P(first);
            val_len = Z_STRLEN_P(first);
        }
    }
    /* Match a leading "text/event-stream" — the handler is allowed to
     * append parameters (charset etc.) but the base type must agree. */
    static const char expected[] = "text/event-stream";
    static const size_t expected_len = sizeof(expected) - 1;
    if (val != NULL && val_len >= expected_len &&
        zend_binary_strcasecmp(val, expected_len, expected, expected_len) == 0) {
        return true;
    }
    zend_throw_exception(http_server_invalid_argument_exception_ce,
        "sseStart(): response already has a non-SSE Content-Type — "
        "remove it before switching to SSE", 0);
    return false;
}

/* Set one header by literal lowercase name. Replaces any prior value.
 * Wraps add_header_value to avoid the runtime zend_string round-trip
 * for these three constants. */
static void sse_set_header_literal(HashTable *headers,
                                   const char *name, size_t name_len,
                                   const char *value, size_t value_len)
{
    zend_string *const key = zend_string_init(name, name_len, 0);
    zval v;
    ZVAL_STRINGL(&v, value, value_len);
    zend_hash_update(headers, key, &v);
    zend_string_release(key);
}

/* Idempotent SSE init: validate Content-Type, set the three canonical
 * headers, switch the response into streaming mode at the PHP boundary.
 * Does NOT emit anything on the wire — the first append_chunk in
 * sseEvent / sseComment / end() drives the wire commit through the
 * protocol-specific stream ops. Returns false on error (exception set). */
static bool sse_ensure_started(http_response_object *response)
{
    if (response->streaming) {
        return true;
    }
    if (response->closed) {
        zend_throw_exception(http_server_runtime_exception_ce,
            "Cannot start SSE on a closed response", 0);
        return false;
    }
    if (UNEXPECTED(response->stream_ops == NULL)) {
        zend_throw_exception(http_server_runtime_exception_ce,
            "SSE requires a streaming-capable response (no stream ops "
            "installed — response is detached from a connection)", 0);
        return false;
    }
    if (!sse_validate_content_type(response)) {
        return false;
    }

    sse_set_header_literal(response->headers,
        "content-type", sizeof("content-type") - 1,
        "text/event-stream", sizeof("text/event-stream") - 1);
    sse_set_header_literal(response->headers,
        "cache-control", sizeof("cache-control") - 1,
        "no-cache, no-transform", sizeof("no-cache, no-transform") - 1);
    /* nginx-specific: disables proxy_buffering for this response.
     * Harmless on protocols / proxies that don't recognise it. */
    sse_set_header_literal(response->headers,
        "x-accel-buffering", sizeof("x-accel-buffering") - 1,
        "no", sizeof("no") - 1);

    response->streaming    = true;
    response->committed    = true;
    response->headers_sent = true;
    return true;
}

/* Append "<prefix>: <value-up-to-newline-or-eos>\n" for each line of
 * `value`. Treats CRLF / CR / LF as line separators (WHATWG §9.2 says
 * input may use any; we emit LF). Used for the data: field which is
 * the only one that may legally contain multiple lines. */
static void sse_append_field_multiline(smart_str *out,
                                       const char *prefix, size_t prefix_len,
                                       const char *value, size_t value_len)
{
    size_t i = 0;
    while (i <= value_len) {
        /* Find end of current line (LF / CR / CRLF / EOS). */
        size_t line_start = i;
        while (i < value_len && value[i] != '\n' && value[i] != '\r') {
            i++;
        }
        smart_str_appendl(out, prefix, prefix_len);
        smart_str_appendl(out, ": ", 2);
        if (i > line_start) {
            smart_str_appendl(out, value + line_start, i - line_start);
        }
        smart_str_appendc(out, '\n');
        if (i >= value_len) {
            break;
        }
        /* Skip the terminator we found. CRLF counts as one. */
        if (value[i] == '\r' && i + 1 < value_len && value[i + 1] == '\n') {
            i += 2;
        } else {
            i++;
        }
        if (i == value_len) {
            /* Trailing terminator — emit one final empty data line so
             * the consumer sees the trailing newline the handler intended. */
            smart_str_appendl(out, prefix, prefix_len);
            smart_str_appendl(out, ": \n", 3);
            break;
        }
    }
}

/* Append "<prefix>: <value>\n". Caller must have already verified
 * `value` contains no \r / \n. */
static inline void sse_append_field_single(smart_str *out,
                                           const char *prefix, size_t prefix_len,
                                           const char *value, size_t value_len)
{
    smart_str_appendl(out, prefix, prefix_len);
    smart_str_appendl(out, ": ", 2);
    if (value_len > 0) {
        smart_str_appendl(out, value, value_len);
    }
    smart_str_appendc(out, '\n');
}

/* Push a finalised event payload through the installed stream ops.
 * Mirrors HttpResponse::send() — same 499 behaviour on stream-dead. */
static int sse_dispatch_payload(http_response_object *response,
                                zend_string *payload)
{
    const int rc = response->stream_ops->append_chunk(
        response->stream_ctx, payload);
    if (rc == HTTP_STREAM_APPEND_STREAM_DEAD) {
        zend_throw_exception_ex(http_exception_ce, 499,
            "stream closed by peer");
    }
    return rc;
}

/* {{{ proto HttpResponse::sseStart(): static */
ZEND_METHOD(TrueAsync_HttpResponse, sseStart)
{
    ZEND_PARSE_PARAMETERS_NONE();

    http_response_object *const response = Z_HTTP_RESPONSE_P(ZEND_THIS);

    if (response->streaming) {
        zend_throw_exception(http_server_runtime_exception_ce,
            "sseStart(): response is already in streaming mode", 0);
        return;
    }
    if (!sse_ensure_started(response)) {
        return;
    }
    RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
}
/* }}} */

/* {{{ proto HttpResponse::sseEvent(?string $data = null, ?string $event = null,
 *                                  ?string $id = null, ?int $retry = null): static */
ZEND_METHOD(TrueAsync_HttpResponse, sseEvent)
{
    zend_string *data  = NULL;
    zend_string *event = NULL;
    zend_string *id    = NULL;
    zend_long    retry = 0;
    bool         retry_is_null = true;

    ZEND_PARSE_PARAMETERS_START(0, 4)
        Z_PARAM_OPTIONAL
        Z_PARAM_STR_OR_NULL(data)
        Z_PARAM_STR_OR_NULL(event)
        Z_PARAM_STR_OR_NULL(id)
        Z_PARAM_LONG_OR_NULL(retry, retry_is_null)
    ZEND_PARSE_PARAMETERS_END();

    http_response_object *const response = Z_HTTP_RESPONSE_P(ZEND_THIS);

    if (response->closed) {
        zend_throw_exception(http_server_runtime_exception_ce,
            "Cannot sseEvent() on a closed response", 0);
        return;
    }

    /* Single-line fields may not contain CR / LF — those are field /
     * record separators in the SSE grammar and would let an attacker
     * (or careless code) inject extra fields or terminate the event
     * early. Throw so the bug surfaces in development. */
    if (sse_string_has_newline(event)) {
        zend_throw_exception(http_server_invalid_argument_exception_ce,
            "sseEvent(): $event must not contain CR or LF", 0);
        return;
    }
    if (sse_string_has_newline(id)) {
        zend_throw_exception(http_server_invalid_argument_exception_ce,
            "sseEvent(): $id must not contain CR or LF", 0);
        return;
    }
    /* WHATWG §9.2: the U+0000 NULL byte in `id` causes the entire id
     * field to be ignored by the parser. Reject up front. */
    if (id != NULL && memchr(ZSTR_VAL(id), '\0', ZSTR_LEN(id)) != NULL) {
        zend_throw_exception(http_server_invalid_argument_exception_ce,
            "sseEvent(): $id must not contain NUL bytes", 0);
        return;
    }
    if (!retry_is_null && retry < 0) {
        zend_throw_exception(http_server_invalid_argument_exception_ce,
            "sseEvent(): $retry must be a non-negative integer", 0);
        return;
    }

    if (!sse_ensure_started(response)) {
        return;
    }

    /* Build the event record. Order doesn't matter to spec-compliant
     * parsers, but `id` / `event` / `retry` before `data` is the
     * convention everyone uses. Trailing blank line terminates. */
    smart_str payload = {0};
    if (id != NULL) {
        sse_append_field_single(&payload, "id", 2,
                                ZSTR_VAL(id), ZSTR_LEN(id));
    }
    if (event != NULL) {
        sse_append_field_single(&payload, "event", 5,
                                ZSTR_VAL(event), ZSTR_LEN(event));
    }
    if (!retry_is_null) {
        char buf[32];
        const int n = snprintf(buf, sizeof(buf), ZEND_LONG_FMT, retry);
        if (n > 0) {
            sse_append_field_single(&payload, "retry", 5, buf, (size_t)n);
        }
    }
    if (data != NULL) {
        sse_append_field_multiline(&payload, "data", 4,
                                   ZSTR_VAL(data), ZSTR_LEN(data));
    }
    smart_str_appendc(&payload, '\n');
    smart_str_0(&payload);

    if (payload.s == NULL) {
        /* All four arguments were null — nothing to send. Still a
         * legal call on a started stream; act as a no-op. */
        RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
    }

    /* Hand ownership to append_chunk — it takes ownership of the ref. */
    sse_dispatch_payload(response, payload.s);
    /* payload.s ref consumed by append_chunk regardless of outcome. */
    if (EG(exception)) {
        return;
    }
    RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
}
/* }}} */

/* {{{ proto HttpResponse::sseComment(string $text = ""): static */
ZEND_METHOD(TrueAsync_HttpResponse, sseComment)
{
    zend_string *text = NULL;

    ZEND_PARSE_PARAMETERS_START(0, 1)
        Z_PARAM_OPTIONAL
        Z_PARAM_STR(text)
    ZEND_PARSE_PARAMETERS_END();

    http_response_object *const response = Z_HTTP_RESPONSE_P(ZEND_THIS);

    if (response->closed) {
        zend_throw_exception(http_server_runtime_exception_ce,
            "Cannot sseComment() on a closed response", 0);
        return;
    }
    if (sse_string_has_newline(text)) {
        zend_throw_exception(http_server_invalid_argument_exception_ce,
            "sseComment(): $text must not contain CR or LF", 0);
        return;
    }
    if (!sse_ensure_started(response)) {
        return;
    }

    smart_str payload = {0};
    smart_str_appendc(&payload, ':');
    if (text != NULL && ZSTR_LEN(text) > 0) {
        smart_str_appendc(&payload, ' ');
        smart_str_append(&payload, text);
    }
    smart_str_appendl(&payload, "\n\n", 2);
    smart_str_0(&payload);

    sse_dispatch_payload(response, payload.s);
    if (EG(exception)) {
        return;
    }
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

    zend_object_std_dtor(&response->std);
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

/* Format response as HTTP string */
zend_string *http_response_format(zend_object *obj)
{
    http_response_object *response = http_response_from_obj(obj);
    smart_str result = {0};

    emit_status_line(&result, response);

    /* Add Content-Length if body exists and not already set. Use
     * zend_hash_str_exists to skip the zend_string alloc/release
     * round-trip on the literal name lookup. */
    smart_str_0(&response->body);
    size_t body_len = response->body.s ? ZSTR_LEN(response->body.s) : 0;

    if (!zend_hash_str_exists(response->headers, "content-length",
                              sizeof("content-length") - 1)) {
        char len_str[32];
        snprintf(len_str, sizeof(len_str), "%zu", body_len);
        smart_str_appends(&result, "Content-Length: ");
        smart_str_appends(&result, len_str);
        smart_str_appends(&result, "\r\n");
    }

    /* Headers — flat IS_STRING avoids nested foreach for the
     * single-value common case (§4.4 perf). */
    zend_string *name;
    zval *values;
    ZEND_HASH_FOREACH_STR_KEY_VAL(response->headers, name, values) {
        if (UNEXPECTED(name == NULL)) continue;
        if (EXPECTED(Z_TYPE_P(values) == IS_STRING)) {
            smart_str_append(&result, name);
            smart_str_appends(&result, ": ");
            smart_str_append(&result, Z_STR_P(values));
            smart_str_appends(&result, "\r\n");
        } else if (Z_TYPE_P(values) == IS_ARRAY) {
            zval *val;
            ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(values), val) {
                smart_str_append(&result, name);
                smart_str_appends(&result, ": ");
                smart_str_append(&result, Z_STR_P(val));
                smart_str_appends(&result, "\r\n");
            } ZEND_HASH_FOREACH_END();
        }
    } ZEND_HASH_FOREACH_END();

    /* End of headers */
    smart_str_appends(&result, "\r\n");

    /* Body */
    if (response->body.s && ZSTR_LEN(response->body.s) > 0) {
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
