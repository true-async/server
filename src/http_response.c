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
#include "http_response_internal.h"
#include "smart_str_scalable.h"

/* Include generated arginfo */
#include "../stubs/HttpResponse.php_arginfo.h"

/* http_response_object layout, http_response_from_obj() and
 * response_clear_body_view() live in http_response_internal.h so
 * the wire-format (src/http1/http1_format.c) and server-side C-API
 * (src/http_response_server_api.c) TUs can poke fields directly. */

/* Class entry */
zend_class_entry *http_response_ce;
static zend_object_handlers http_response_handlers;

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

    if (response->send_file_req != NULL) {
        zend_throw_exception(http_server_runtime_exception_ce,
            "Response is sealed by sendFile() — no further mutation allowed", 0);
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
    (void)return_value;
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

        zend_string *lname = zend_string_tolower(name);
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
    response_clear_body_view(response);
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

    if (response->body_view != NULL) {
        /* Borrowed body is immutable from the response side — a refcount
         * bump is safe (no realloc/free path mutates it in place). */
        RETURN_STR_COPY(response->body_view);
    }

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
    response_clear_body_view(response);
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
    (void)return_value;
    zval *stream;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_ZVAL(stream)
    ZEND_PARSE_PARAMETERS_END();

    /* TODO: Implement body stream support */
    zend_throw_exception(http_server_runtime_exception_ce,
        "Body stream support is not yet implemented", 0);
}
/* }}} */

/* Wire the per-request JSON-encode default into a freshly-dispatched
 * response. Called from H1/H2/H3 dispatch alongside compression_attach;
 * exported (non-static) so the protocol TUs can reach it without
 * publishing the response struct layout. */
void http_response_set_default_json_flags(zend_object *obj, uint32_t flags)
{
    if (UNEXPECTED(obj == NULL)) return;
    http_response_object *response = http_response_from_obj(obj);
    response->default_json_flags = flags;
}

/* {{{ proto HttpResponse::json(array|string $data, int $status = 200, int $flags = 0): static
 *
 * Set the response body to a JSON payload.
 *
 *   $data: array|object → encoded via php_json_encode_ex.
 *          string       → shipped as-is (caller already has JSON bytes,
 *                         e.g. from a cache or a JSON_UNESCAPED build
 *                         done elsewhere). The string is NOT validated;
 *                         that contract is on the caller.
 *   $status: HTTP status code. Default 200.
 *   $flags:  JSON_* bitmask. 0 → use the per-server default
 *            (HttpServerConfig::setJsonEncodeFlags).
 *            JSON_THROW_ON_ERROR is silently stripped — encode failure
 *            yields a 500 JSON error body, not a propagated exception
 *            (handlers never need to wrap json() in try/catch).
 *
 * Content-Type is set to application/json only if the handler did NOT
 * already set one — this lets handlers ship application/problem+json,
 * application/vnd.api+json, etc. just by calling setHeader() before
 * json(). */
ZEND_METHOD(TrueAsync_HttpResponse, json)
{
    zval     *data;
    zend_long status = 200;
    zend_long flags  = 0;

    ZEND_PARSE_PARAMETERS_START(1, 3)
        Z_PARAM_ZVAL(data)
        Z_PARAM_OPTIONAL
        Z_PARAM_LONG(status)
        Z_PARAM_LONG(flags)
    ZEND_PARSE_PARAMETERS_END();

    http_response_object *response = Z_HTTP_RESPONSE_P(ZEND_THIS);

    if (response_check_closed(response)) {
        return;
    }

    if (UNEXPECTED(status < 100 || status > 599)) {
        zend_throw_exception(http_server_invalid_argument_exception_ce,
            "HTTP status code must be between 100 and 599", 0);
        return;
    }

    /* Per-call $flags override the server default; THROW_ON_ERROR stripped. */
    int effective_flags = (int)((flags != 0 ? (uint32_t)flags
                                            : response->default_json_flags)
                                 & ~PHP_JSON_THROW_ON_ERROR);

    /* Set Content-Type: application/json only if the handler did not
     * already specify one — preserves application/problem+json,
     * application/vnd.api+json, etc. */
    if (!zend_hash_str_exists(response->headers, "content-type", sizeof("content-type") - 1)) {
        zval ct;
        ZVAL_STRING(&ct, "application/json");
        zend_string *ct_name = zend_string_init("content-type", sizeof("content-type") - 1, 0);
        add_header_value(response->headers, ct_name, &ct, true);
        zend_string_release(ct_name);
        zval_ptr_dtor(&ct);
    }

    response->status_code = (int)status;

    /* Pre-encoded passthrough: caller hands us JSON bytes directly. */
    if (Z_TYPE_P(data) == IS_STRING) {
        response_clear_body_view(response);
        smart_str_free(&response->body);
        smart_str_appendl(&response->body, Z_STRVAL_P(data), Z_STRLEN_P(data));
        smart_str_0(&response->body);
        RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
    }

    /* Encode into a fresh smart_str — only swap into the response body
     * on success, so an aborted encode leaves the previous body intact. */
    smart_str encoded = {0};
    const zend_result rc = php_json_encode_ex(&encoded, data, effective_flags,
                                              PHP_JSON_PARSER_DEFAULT_DEPTH);
    smart_str_0(&encoded);

    if (UNEXPECTED(rc == FAILURE)) {
        /* Build a controlled 500 response body so the client never sees
         * a partial encode (which is what php_json_encode would have
         * left in `encoded` on failure without PARTIAL_OUTPUT_ON_ERROR). */
        smart_str_free(&encoded);
        response_clear_body_view(response);
        smart_str_free(&response->body);
        static const char err_body[] = "{\"error\":\"json encoding failed\"}";
        smart_str_appendl(&response->body, err_body, sizeof(err_body) - 1);
        smart_str_0(&response->body);
        response->status_code = 500;
        RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
    }

    response_clear_body_view(response);
    smart_str_free(&response->body);
    response->body = encoded;

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
    response_clear_body_view(response);
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

    http_response_object *response = Z_HTTP_RESPONSE_P(ZEND_THIS);

    if (response->closed) {
        zend_throw_exception(http_server_runtime_exception_ce,
            "Response already closed — cannot send() after end()", 0);
        return;
    }

    if (response->send_file_req != NULL) {
        zend_throw_exception(http_server_runtime_exception_ce,
            "Response is sealed by sendFile() — no further mutation allowed", 0);
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

/* {{{ proto HttpResponse::sendable(): bool
 *
 * Advisory, non-blocking backpressure check. Returns true when send()
 * would accept a chunk without suspending the handler coroutine — the
 * per-stream staging buffer has room. Returns false when send() would
 * block on backpressure, or when the response is closed / sealed by
 * sendFile() / not streaming-capable.
 *
 * send() is always safe to call regardless; sendable() just lets a
 * handler do other work instead of blocking on a slow peer. */
ZEND_METHOD(TrueAsync_HttpResponse, sendable)
{
    ZEND_PARSE_PARAMETERS_NONE();

    http_response_object *response = Z_HTTP_RESPONSE_P(ZEND_THIS);

    if (response->closed
        || response->send_file_req != NULL
        || response->stream_ops == NULL) {
        RETURN_FALSE;
    }

    /* Protocol without a userspace staging ring (HTTP/1, paced by the
     * kernel socket buffer) leaves the op NULL — report writable. */
    if (response->stream_ops->sendable == NULL) {
        RETURN_TRUE;
    }

    RETURN_BOOL(response->stream_ops->sendable(response->stream_ctx));
}
/* }}} */

/* {{{ proto HttpResponse::sendFile(string $path, ?SendFileOptions $options = null): void
 *
 * Records a path + options pair on the response and seals it. Returns
 * immediately; the dispose path calls into the per-protocol sendfile
 * FSM. Path must be absolute; the handler is treated as the trust
 * boundary (no symlink/dotfile policy is applied here). */
ZEND_METHOD(TrueAsync_HttpResponse, sendFile)
{
    zend_string *path;
    zval        *options_zv = NULL;

    ZEND_PARSE_PARAMETERS_START(1, 2)
        Z_PARAM_STR(path)
        Z_PARAM_OPTIONAL
        Z_PARAM_OBJECT_OF_CLASS_OR_NULL(options_zv, http_send_file_options_ce)
    ZEND_PARSE_PARAMETERS_END();

    http_response_object *response = Z_HTTP_RESPONSE_P(ZEND_THIS);

    if (response->closed) {
        zend_throw_exception(http_server_runtime_exception_ce,
            "Response has already been closed", 0);
        return;
    }

    if (response->streaming || response->headers_sent) {
        zend_throw_exception(http_server_runtime_exception_ce,
            "sendFile(): headers already committed", 0);
        return;
    }

    if (response->send_file_req != NULL) {
        zend_throw_exception(http_server_runtime_exception_ce,
            "Response is sealed by sendFile() — sendFile already called", 0);
        return;
    }

    if (UNEXPECTED(ZSTR_LEN(path) == 0)) {
        zend_throw_exception(http_server_runtime_exception_ce,
            "sendFile(): path must not be empty", 0);
        return;
    }

    if (UNEXPECTED(ZSTR_VAL(path)[0] != '/')) {
        zend_throw_exception(http_server_runtime_exception_ce,
            "sendFile(): path must be absolute", 0);
        return;
    }

    http_send_file_request_t *req = ecalloc(1, sizeof(*req));
    req->path = zend_string_copy(path);
    http_send_file_options_snapshot(
        options_zv != NULL ? Z_OBJ_P(options_zv) : NULL,
        &req->opts);

    /* Per-call validation. RFC says no CR/LF in header values; status
     * range matches the rest of the API. */
    const http_send_file_options_t *opts = &req->opts;

    if (opts->status != 0 && (opts->status < 100 || opts->status > 599)) {
        http_send_file_request_free(req);
        zend_throw_exception(http_server_runtime_exception_ce,
            "sendFile(): status must be between 100 and 599", 0);
        return;
    }
#define HAS_CRLF(zs) ((zs) != NULL && \
        (memchr(ZSTR_VAL(zs), '\r', ZSTR_LEN(zs)) != NULL \
         || memchr(ZSTR_VAL(zs), '\n', ZSTR_LEN(zs)) != NULL))

    if (HAS_CRLF(opts->content_type) || HAS_CRLF(opts->download_name)
        || HAS_CRLF(opts->cache_control)) {
        http_send_file_request_free(req);
        zend_throw_exception(http_server_runtime_exception_ce,
            "sendFile(): option strings must not contain CR or LF", 0);
        return;
    }
#undef HAS_CRLF

    response->send_file_req = req;

    /* Sendfile body bypasses the compression module — never wrap. */
#ifdef HAVE_HTTP_COMPRESSION
    {
        extern void http_compression_mark_no_compression(zend_object *);
        http_compression_mark_no_compression(Z_OBJ_P(ZEND_THIS));
    }
#endif
}
/* }}} */

/* {{{ proto HttpResponse::end(?string $data = null): void */
ZEND_METHOD(TrueAsync_HttpResponse, end)
{
    (void)return_value;
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

    if (response->send_file_req != NULL) {
        zend_throw_exception(http_server_runtime_exception_ce,
            "Response is sealed by sendFile() — no further mutation allowed", 0);
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
        response_clear_body_view(response);
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
    response->default_json_flags = 0;
    response->send_file_req = NULL;
    response->socket_fd = SOCK_ERR;
    memset(&response->body, 0, sizeof(smart_str));
    response->body_view = NULL;

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

    response_clear_body_view(response);
    smart_str_free(&response->body);

    /* Aborted-request safety: dispose never picked up the descriptor. */
    if (response->send_file_req != NULL) {
        http_send_file_request_free(response->send_file_req);
        response->send_file_req = NULL;
    }

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

/* Read-only accessors used by the H1 static-file delivery op
 * (src/http1/http1_sendfile.c) to serialize the head verbatim
 * without going through http_response_format (which auto-adds
 * Content-Length and runs the compression hook — neither is
 * appropriate when the static handler has already decided every
 * header on the wire). */
zend_string *http_response_get_body_string(zend_object *obj)
{
    smart_str *b = &http_response_from_obj(obj)->body;

    if (b->s == NULL || ZSTR_LEN(b->s) == 0) {
        return NULL;
    }

    smart_str_0(b);
    return b->s;
}

/* sendFile descriptor accessors used by the dispose path (issue #13). */
http_send_file_request_t *http_response_take_send_file(zend_object *obj)
{
    http_response_object *r = http_response_from_obj(obj);
    http_send_file_request_t *req = r->send_file_req;
    r->send_file_req = NULL;
    return req;
}

bool http_response_has_send_file(zend_object *obj)
{
    return http_response_from_obj(obj)->send_file_req != NULL;
}

/* {{{ http_response_class_register */
void http_response_class_register(void)
{
    http_response_ce = register_class_TrueAsync_HttpResponse();
    http_response_ce->create_object = http_response_create;

    memcpy(&http_response_handlers, &std_object_handlers, sizeof(zend_object_handlers));
    http_response_handlers.offset = offsetof(http_response_object, std);
    http_response_handlers.free_obj = http_response_free;
    http_response_handlers.clone_obj = NULL;
}
/* }}} */
