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
#include "Zend/zend_virtual_cwd.h" /* IS_ABSOLUTE_PATH */
#include "php_http_server.h"
#include "http_response_internal.h"
#include "smart_str_scalable.h"
#include "grpc/grpc.h"

#ifdef HAVE_HTTP_COMPRESSION
# include "compression/http_compression_response.h"
#endif

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
 * no-longer-mutable in four states, tested in the order that decides which one
 * the handler is told about:
 *  1. aborted   — abort() finished the response, whatever the transport could
 *                 do about the body; named ahead of closed so the handler
 *                 hears about its own call rather than about end().
 *  2. closed    — end() has been called; nothing further is possible.
 *  3. streaming — write() has been called; status + headers are
 *                 committed on the wire. Trailers are still allowed
 *                 (they're post-DATA) and go through separate
 *                 non-guarded setters — see setTrailer/setTrailers.
 *  4. sealed    — sendFile() owns the body, so the header block is spoken for
 *                 too. */
static inline bool response_check_closed(const http_response_object *response)
{
    if (response->aborted) {
        zend_throw_exception(http_server_runtime_exception_ce,
            "Cannot modify response after abort() has been called", 0);
        return true;
    }

    if (response->closed) {
        zend_throw_exception(http_server_runtime_exception_ce,
            "Cannot modify response after end() has been called", 0);
        return true;
    }

    if (response->streaming) {
        zend_throw_exception(http_server_runtime_exception_ce,
            "Cannot modify response — headers already committed by write()", 0);
        return true;
    }

    if (response->send_file_req != NULL) {
        zend_throw_exception(http_server_runtime_exception_ce,
            "Response is sealed by sendFile() — no further mutation allowed", 0);
        return true;
    }

    return false;
}

/* Trailers go out after the body, so setting them post-commit is legal;
 * only end() and sendFile() sealing forbid them. */
static inline bool response_check_trailer_sealed(const http_response_object *response)
{
    if (response->aborted) {
        zend_throw_exception(http_server_runtime_exception_ce,
            "Cannot set trailers after abort() has been called", 0);
        return true;
    }

    if (response->closed) {
        zend_throw_exception(http_server_runtime_exception_ce,
            "Cannot set trailers after end() has been called", 0);
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

/* The statuses a handler may give a final response, and why the range starts
 * at 200. A 1xx is interim (RFC 9110 §15.2): the client reads it and goes on
 * waiting for the answer, which nothing will send — the request is over when
 * the handler returns. Every writer of status_code asks this, so the rule
 * cannot be walked around through json() or sendFile(). @p what names the call
 * in the message. */
static bool response_status_is_final(const zend_long status, const char *what)
{
    if (status >= 200 && status <= 599) {
        return true;
    }

    if (status >= 100 && status < 200) {
        zend_throw_exception_ex(http_server_invalid_argument_exception_ce, 0,
            "%s: status %d is interim (RFC 9110 §15.2) and cannot be a final "
            "response: the client would wait for one that never comes",
            what, (int) status);
        return false;
    }

    zend_throw_exception_ex(http_server_invalid_argument_exception_ce, 0,
        "%s: HTTP status code must be between 200 and 599, got %d",
        what, (int) status);
    return false;
}

/* Whether a byte may stand in a header field value: everything visible, plus
 * the horizontal tab (RFC 9110 §5.5 — field-vchar is VCHAR / obs-text, and
 * SP / HTAB may separate them). What this excludes is the point of it: a CR or
 * an LF ends the header block, so the bytes behind one are read as further
 * fields and, past a blank line, as a second response the server never sent
 * (CWE-113). */
static bool header_value_byte_allowed(const unsigned char c)
{
    return c >= 0x20 ? c != 0x7F : c == '\t';
}

/* RFC 9110 §5.6.2 token — the grammar a field name has to satisfy. A space or
 * a colon in a name splits the line the same way a CR does. */
static bool header_name_char_allowed(const unsigned char c)
{
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
        return true;
    }

    return memchr("!#$%&'*+-.^_`|~", c, sizeof("!#$%&'*+-.^_`|~") - 1) != NULL;
}

/* Refuses a field the server cannot put on the wire as one field, and says
 * which one. The reason phrase is cleaned instead, because `reset_to_error`
 * runs inside exception handling with no one left to tell; a header is set
 * while the handler is still running, so it is told. */
static bool header_field_check(zend_string *name, const zval *value)
{
    if (ZSTR_LEN(name) == 0) {
        zend_throw_exception(http_server_invalid_argument_exception_ce,
            "Header name must not be empty", 0);
        return false;
    }

    for (size_t i = 0; i < ZSTR_LEN(name); i++) {
        if (!header_name_char_allowed((unsigned char) ZSTR_VAL(name)[i])) {
            zend_throw_exception_ex(http_server_invalid_argument_exception_ce, 0,
                "Header name \"%s\" is not a token: byte 0x%02X at offset %zu is not allowed",
                ZSTR_VAL(name), (unsigned char) ZSTR_VAL(name)[i], i);
            return false;
        }
    }

    /* The bytes checked are the bytes stored, which is why the value is
     * resolved first: storage converts, and an object's __toString() is the
     * shape that carries request data into a header — a PSR-7 URI built from a
     * query parameter reaches setHeader('Location', $uri) as an object, and
     * checking the zval's type instead of its bytes would let it past. */
    zend_string *str = zval_try_get_string((zval *) value);

    if (str == NULL) {
        return false;   /* conversion threw; the message names the type */
    }

    for (size_t i = 0; i < ZSTR_LEN(str); i++) {
        if (!header_value_byte_allowed((unsigned char) ZSTR_VAL(str)[i])) {
            zend_throw_exception_ex(http_server_invalid_argument_exception_ce, 0,
                "Header \"%s\" carries byte 0x%02X at offset %zu, which cannot "
                "stand in a field value",
                ZSTR_VAL(name), (unsigned char) ZSTR_VAL(str)[i], i);
            zend_string_release(str);
            return false;
        }
    }

    /* RFC 9110 §5.5: a field value has no leading or trailing whitespace, and
     * a sender must not generate one that does. Recipients strip it, so this
     * refuses a value that would arrive as something other than what was set. */
    if (ZSTR_LEN(str) > 0) {
        const char first = ZSTR_VAL(str)[0];
        const char last  = ZSTR_VAL(str)[ZSTR_LEN(str) - 1];

        if (first == ' ' || first == '\t' || last == ' ' || last == '\t') {
            zend_throw_exception_ex(http_server_invalid_argument_exception_ce, 0,
                "Header \"%s\" has leading or trailing whitespace, which RFC 9110 "
                "§5.5 forbids a sender to generate", ZSTR_VAL(name));
            zend_string_release(str);
            return false;
        }
    }

    zend_string_release(str);
    return true;
}

/* The two fields the server states for itself, and what a handler setting one
 * is answered with. `connection` is read as a request rather than copied as
 * bytes: `close` records an intent the HTTP/1 dispose path turns into a closed
 * socket, `keep-alive` is dropped because it is what the server was going to
 * say anyway, and anything else names a connection option the server does not
 * implement. `transfer-encoding` is refused unless it names the chunked coding
 * the server would apply anyway. Dropping a `gzip` instead would put encoded
 * bytes on the wire with no coding declared anywhere, which is a corrupt
 * download rather than a header worth second-guessing.
 *
 * `content-length` is deliberately not here: a declared length is a contract
 * the streaming path audits, and the buffered path replaces it with the count
 * it is sending.
 *
 * Returns true when the field was handled here and must not be stored, whether
 * it was taken or refused; an exception is pending in the second case. */
static bool response_take_server_field(http_response_object *response,
                                       zend_string *lower_name, const zval *value)
{
    const bool is_connection = zend_string_equals_literal(lower_name, "connection");

    if (!is_connection && !zend_string_equals_literal(lower_name, "transfer-encoding")) {
        return false;
    }

    zend_string *str = zval_try_get_string((zval *) value);

    if (str == NULL) {
        return true;    /* conversion threw */
    }

    const char *val = ZSTR_VAL(str);
    const size_t len = ZSTR_LEN(str);

    if (is_connection) {
        if (len == 5 && zend_binary_strcasecmp(val, len, "close", 5) == 0) {
            response->handler_wants_close = true;
        } else if (len != 10 || zend_binary_strcasecmp(val, len, "keep-alive", 10) != 0) {
            /* keep-alive is dropped rather than refused: it is what the server
             * was about to say anyway, and the shape that sets it is a handler
             * copying an upstream response's headers wholesale. Refusing that
             * would turn a correct response into a 500 over a field the server
             * ignores. Anything else names a connection option the server does
             * not implement, and silence would be a promise it cannot keep. */
            zend_throw_exception_ex(http_server_invalid_argument_exception_ce, 0,
                "Connection: %s is the server's to decide. Only \"close\" can be "
                "asked for, which retires the connection after this response on "
                "HTTP/1; HTTP/2 and HTTP/3 multiplex, so one response never "
                "retires their connection", val);
        }
    } else if (len != 7 || zend_binary_strcasecmp(val, len, "chunked", 7) != 0) {
        zend_throw_exception_ex(http_server_invalid_argument_exception_ce, 0,
            "Transfer-Encoding: %s cannot be applied by the server, and the framing "
            "is not the handler's to state; the server negotiates a content coding "
            "of its own",
            val);
    }
    /* `chunked` falls through: it is the framing an undeclared HTTP/1.1 stream
     * gets anyway, so a handler stating the obvious is not made to care which
     * path its response takes. */

    zend_string_release(str);
    return true;
}

/* True means the caller must not store the field: it was either taken or
 * refused, and an exception is pending in the second case. */
static bool response_take_header_for_server(http_response_object *response,
                                            zend_string *name, zval *value)
{
    zend_string *lower = zend_string_tolower(name);
    const bool taken = response_take_server_field(response, lower, value);
    zend_string_release(lower);
    return taken;
}

/* Every value a handler offers, checked before any of them is stored: a field
 * set as an array must not be stored half-written when its third element is the bad
 * one. Non-string scalars are converted at storage time and cannot carry a
 * forbidden byte. */
static bool header_values_check(zend_string *name, zval *value)
{
    if (Z_TYPE_P(value) != IS_ARRAY) {
        return header_field_check(name, value);
    }

    zval *item;
    ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(value), item) {
        if (!header_field_check(name, item)) {
            return false;
        }
    } ZEND_HASH_FOREACH_END();

    return true;
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

    if (!response_status_is_final(code, "setStatusCode()")) {
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

    http_response_set_reason_phrase(Z_OBJ_P(ZEND_THIS), ZSTR_VAL(phrase), ZSTR_LEN(phrase));

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

    if (!header_values_check(name, value)) {
        return;
    }

    if (response_take_header_for_server(response, name, value)) {
        RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
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

    if (!header_values_check(name, value)) {
        return;
    }

    if (response_take_header_for_server(response, name, value)) {
        RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
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
    /* The close request is a header the table does not hold, so clearing the
     * table has to clear it too — otherwise a middleware that rebuilds the
     * header set cannot take it back. */
    response->handler_wants_close = false;

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

/* Fold trailers into headers and clear the table (gRPC Trailers-Only reply). */
void http_response_promote_trailers_to_headers(zend_object *obj)
{
    http_response_object *response = http_response_from_obj(obj);

    if (response->trailers == NULL
        || zend_hash_num_elements(response->trailers) == 0) {
        return;
    }

    zend_string *name;
    zval        *val;
    ZEND_HASH_FOREACH_STR_KEY_VAL(response->trailers, name, val) {
        if (name == NULL || Z_TYPE_P(val) != IS_STRING) {
            continue;
        }
        zval copy;
        ZVAL_STR_COPY(&copy, Z_STR_P(val));
        zend_hash_update(response->headers, name, &copy);
    } ZEND_HASH_FOREACH_END();

    zend_hash_clean(response->trailers);
}

/* grpc-web moved the trailers in-body; the HTTP trailer emission must find nothing. */
void http_response_clear_trailers(zend_object *obj)
{
    http_response_object *response = http_response_from_obj(obj);

    if (response->trailers != NULL) {
        zend_hash_clean(response->trailers);
    }
}

/* grpc-status is mandatory on the wire, even on success (0). */
void http_response_ensure_grpc_status(zend_object *obj, int status)
{
    http_response_object *response = http_response_from_obj(obj);

    ensure_trailers_table(response);

    if (zend_hash_str_exists(response->trailers, "grpc-status",
                             sizeof("grpc-status") - 1)) {
        return;
    }

    char buf[16];
    const int len = snprintf(buf, sizeof(buf), "%d", status);
    zval v;
    ZVAL_STRINGL(&v, buf, len);
    zend_hash_str_update(response->trailers, "grpc-status",
                         sizeof("grpc-status") - 1, &v);
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

    if (response_check_trailer_sealed(response)) {
        return;
    }

    /* A trailer is a field on the wire, so it answers to the field grammar the
     * header setters answer to. Today's transports validate it themselves —
     * HTTP/1 emits no trailers at all, and nghttp2 and nghttp3 have their own
     * checks — which is exactly why the guard belongs here rather than in the
     * emitters: gRPC puts an exception message into `grpc-message`, and the day
     * a chunked-trailer emitter arrives on HTTP/1 that is CWE-113 with nothing
     * in its way. */
    {
        zval value_zv;
        ZVAL_STR(&value_zv, value);

        if (!header_field_check(name, &value_zv)) {
            return;
        }
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

    if (response_check_trailer_sealed(response)) {
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

    if (response_check_trailer_sealed(response)) {
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

/* {{{ proto HttpResponse::appendBody(string $data): static */
ZEND_METHOD(TrueAsync_HttpResponse, appendBody)
{
    zend_string *data;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_STR(data)
    ZEND_PARSE_PARAMETERS_END();

    http_response_object *response = Z_HTTP_RESPONSE_P(ZEND_THIS);

    if (response_check_closed(response)) {
        return;
    }

    /* Buffered-mode incremental API: the handler calls it N times and the
     * full body goes out on end(). Size is unknown up front — scalable-grow
     * flips to doubling above 2 MiB so a handler appending a 256 MiB body
     * doesn't take 40 k mremap calls. See smart_str_scalable.h. */
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

    if (UNEXPECTED(!response_status_is_final(status, "json()"))) {
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

    /* The URL is handler data, and request data behind it more often than not,
     * so it answers to the same field-value rule as setHeader(). Checked before
     * the status is assigned: a refused redirect leaves the response as it was,
     * which is what lets the handler answer with something else. */
    zval location;
    ZVAL_STR_COPY(&location, url);
    zend_string *header_name = zend_string_init("location", sizeof("location") - 1, 0);

    if (!header_field_check(header_name, &location)) {
        zend_string_release(header_name);
        zval_ptr_dtor(&location);
        return;
    }

    response->status_code = (int)status;
    add_header_value(response->headers, header_name, &location, true);
    zend_string_release(header_name);
    zval_ptr_dtor(&location);

    RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
}
/* }}} */

/* True when setBody()/appendBody()/json()/html() left bytes waiting for end().
 * An empty buffer does not count: setBody('') commits the handler to nothing. */
static bool response_has_buffered_body(const http_response_object *response)
{
    if (response->body_view != NULL) {
        return ZSTR_LEN(response->body_view) > 0;
    }

    return response->body.s != NULL && ZSTR_LEN(response->body.s) > 0;
}

/* Guards shared by every streaming entry point, so write() and tryWrite()
 * cannot drift apart. Returns true after throwing; `method` names the caller
 * in the message. */
static bool response_check_stream_usable(const http_response_object *response,
                                         const char *method, const bool emits)
{
    if (response->aborted) {
        zend_throw_exception_ex(http_server_runtime_exception_ce, 0,
            "Response already closed — cannot %s() after abort()", method);
        return true;
    }

    if (response->closed) {
        zend_throw_exception_ex(http_server_runtime_exception_ce, 0,
            "Response already closed — cannot %s() after end()", method);
        return true;
    }

    /* RFC 9112 §6.3 rule 1: a 1xx, a 204 and a 304 end at the blank line
     * whatever the header fields say, so a body queued behind one is read by
     * the peer as the next message. The refusal is here, at the call, because
     * the response is still uncommitted and the handler can answer with a
     * status it means. A buffered body is dropped at format time instead:
     * there the status may legitimately have been chosen after the body was
     * built — a conditional GET that renders a representation and then answers
     * 304 — and there is no one left to tell. */
    if (emits && !response_status_carries_body(response->status_code)) {
        zend_throw_exception_ex(http_server_runtime_exception_ce, 0,
            "%s(): status %d carries no body — the message ends at the header block",
            method, response->status_code);
        return true;
    }

    /* SSE owns the framing, so a call that would put its own bytes on the wire
     * is refused. One that only waits is not: an SSE handler refused by
     * trySseEvent() has nowhere else to wait for room. */
    if (response->sse_mode && emits) {
        zend_throw_exception_ex(http_server_runtime_exception_ce, 0,
            "Response is in SSE mode — use sseEvent()/sseComment() instead of %s()", method);
        return true;
    }

    if (response->send_file_req != NULL) {
        zend_throw_exception(http_server_runtime_exception_ce,
            "Response is sealed by sendFile() — no further mutation allowed", 0);
        return true;
    }

    if (response->stream_ops == NULL) {
        /* No stream ops installed — response is detached from a
         * connection (e.g. constructed standalone in user code). */
        zend_throw_exception_ex(http_server_runtime_exception_ce, 0,
            "Response streaming (%s()) is not available on this response", method);
        return true;
    }

    /* A buffered body leaves at end() and the streaming path never reads it,
     * so the two modes are exclusive. response_check_closed() refuses the
     * other direction; this is the same refusal from this side. A call that
     * only waits discards nothing, so it is not refused here either. */
    if (emits && response_has_buffered_body(response)) {
        zend_throw_exception_ex(http_server_runtime_exception_ce, 0,
            "Response already has a buffered body — %s() would discard it. "
            "Choose one mode: setBody()/appendBody() or %s()", method, method);
        return true;
    }

    return false;
}

/* Longest Content-Length this server reads. Nineteen digits is where int64_t
 * runs out mid-range, so the value is range-checked below as well; the digit
 * cap is what keeps the accumulator itself from wrapping. */
#define HTTP_DECLARED_LENGTH_MAX_DIGITS  19

/* Takes the handler's Content-Length, once, into response->declared_length.
 * Returns false after throwing, which is what a value that is not a length
 * gets: dropping it silently is how a truncated body came to read as complete,
 * and at this point the response is still uncommitted, so the throw can still
 * become a status. */
static bool response_take_declared_length(const http_response_object *response,
                                          const char *method, int64_t *out)
{
    const zval *slot = zend_hash_str_find(response->headers, "content-length",
                                          sizeof("content-length") - 1);

    if (slot == NULL) {
        return true;
    }

    if (Z_TYPE_P(slot) != IS_STRING) {
        zend_throw_exception_ex(http_server_runtime_exception_ce, 0,
            "%s(): Content-Length was declared more than once — a body is framed "
            "by one length or by none", method);
        return false;
    }

    const zend_string *value = Z_STR_P(slot);
    const size_t       len   = ZSTR_LEN(value);
    uint64_t           bytes = 0;

    if (len == 0 || len > HTTP_DECLARED_LENGTH_MAX_DIGITS) {
        zend_throw_exception_ex(http_server_runtime_exception_ce, 0,
            "%s(): Content-Length must be a decimal byte count of at most %d digits, "
            "got \"%s\"", method, HTTP_DECLARED_LENGTH_MAX_DIGITS, ZSTR_VAL(value));
        return false;
    }

    for (size_t i = 0; i < len; i++) {
        const char digit = ZSTR_VAL(value)[i];

        if (digit < '0' || digit > '9') {
            zend_throw_exception_ex(http_server_runtime_exception_ce, 0,
                "%s(): Content-Length must be a decimal byte count, got \"%s\"",
                method, ZSTR_VAL(value));
            return false;
        }

        bytes = bytes * 10 + (uint64_t)(digit - '0');
    }

    /* Nineteen digits reach past int64_t, and the sentinel for "none declared"
     * is a negative value: an unchecked cast would turn the largest lengths
     * into no declaration at all, which is the silent drop this guard exists
     * to refuse. */
    if (bytes > (uint64_t)INT64_MAX) {
        zend_throw_exception_ex(http_server_runtime_exception_ce, 0,
            "%s(): Content-Length of %" PRIu64 " is larger than this server frames",
            method, bytes);
        return false;
    }

    *out = (int64_t)bytes;
    return true;
}

/* Guards the declared length on every streaming offer, and takes the
 * declaration on the first one. Returns true after throwing, on the same
 * contract as response_check_stream_usable.
 *
 * The bytes are reserved here rather than counted after the transport
 * answers: every append_chunk suspends, and a second coroutine writing to the
 * same response would otherwise be admitted against a total that is already
 * spoken for. A transport that queues nothing gives the reservation back
 * through response_release_declared_length. */
static bool response_check_declared_length(http_response_object *response,
                                           const char *method, const size_t chunk_len)
{
    int64_t declared = response->declared_length;

    if (!response->streaming) {
        /* gRPC closes its body with a trailer frame the handler never sees, so
         * a length it declared could not describe the wire. A HEAD response
         * sends no body to hold to one, and its length belongs to the buffered
         * path, where it describes what a GET would have returned. The SSE
         * dialect starts its stream elsewhere and keeps the framing it has. */
        if (response->grpc_mode == 0 && !response->is_head
            && response_status_carries_body(response->status_code)
            && !response_take_declared_length(response, method, &declared)) {
            return true;
        }
    }

    if (declared < 0) {
        response->written_length += chunk_len;
        return false;
    }

    if (response->written_length + chunk_len > (uint64_t)declared) {
        zend_throw_exception_ex(http_server_runtime_exception_ce, 0,
            "%s(): body would pass the declared Content-Length of %" PRId64
            " bytes — %" PRIu64 " written, %zu offered", method,
            declared, response->written_length, chunk_len);
        return true;
    }

    /* The response adopts the declaration only once a chunk of its own is
     * accepted. A handler whose first offer is refused goes on to answer with
     * a buffered body, and a length recorded here would then reach the wire
     * beside a body of another size. */
    response->declared_length = declared;
    response->written_length += chunk_len;
    return false;
}

/* Hands back bytes reserved for a transport that then queued nothing: the
 * refusal a non-blocking offer answers with, and every dead stream, where the
 * chunk is released without a byte of it leaving. Keeping them would let a body
 * that never reached the peer satisfy the declared count. */
static void response_release_declared_length(http_response_object *response,
                                             const size_t chunk_len)
{
    response->written_length -= chunk_len;
}

/* First chunk locks headers and switches to streaming mode. After this,
 * setBody / setHeader / setStatusCode throw. */
static void http_response_stream_commit_once(zend_object *obj,
                                             http_response_object *response)
{
    if (response->streaming) {
        return;
    }

    response->streaming = true;
    response->committed = true;
    response->headers_sent = true;
#ifdef HAVE_HTTP_COMPRESSION
    /* A declared length counts the bytes the handler writes, and a codec would
     * put a different number of them on the wire — the encoder deletes the
     * header for exactly that reason. The declaration wins: the handler asked
     * for a body of a known size. */
    if (response->declared_length >= 0) {
        http_compression_mark_no_compression(obj);
    }

    /* Wrap stream_ops with a compressing one if Accept-Encoding +
     * response state allow gzip. Mutates Content-Encoding/Vary on
     * the response so the stream's underlying header-commit picks
     * them up on the next line. */
    http_compression_maybe_install_stream_wrapper(obj);
#endif
}

/* {{{ proto HttpResponse::write(string $chunk): static
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
ZEND_METHOD(TrueAsync_HttpResponse, write)
{
    zend_string *chunk;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_STR(chunk)
    ZEND_PARSE_PARAMETERS_END();

    http_response_object *response = Z_HTTP_RESPONSE_P(ZEND_THIS);

    if (response_check_stream_usable(response, "write", true)) {
        return;
    }

    /* HEAD carries no body (RFC 9110 §9.3.2); the chunk is dropped, and the
     * fact that one was offered is recorded. The response is deliberately not
     * committed: nothing has reached the socket, so the handler's own exception
     * can still become the status, and setHeader() still works. What the flag
     * buys is the buffered formatter's silence: a count taken from the buffer
     * those bytes never reached claims the GET body is empty. A length the
     * handler declared itself survives and describes the body a GET would have
     * returned. */
    if (response->is_head) {
        response->head_streamed = true;
        RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
    }

    /* Before the commit: on the first chunk this is what reads the declared
     * length, and a declaration that is not a length has to be able to become
     * a status while the response can still be given one. */
    if (response_check_declared_length(response, "write", ZSTR_LEN(chunk))) {
        return;
    }

    http_response_stream_commit_once(Z_OBJ_P(ZEND_THIS), response);

    /* Hand ownership of the chunk to the queue — the ops layer
     * takes a refcount. Empty chunks are still accepted (some
     * protocols use them as keepalive signals). */
    zend_string_addref(chunk);
    const int rc = response->stream_ops->append_chunk(
        response->stream_ctx, chunk, false);

    if (rc == HTTP_STREAM_APPEND_STREAM_DEAD) {
        /* Peer aborted between dispatch and now. Emulate the
         * cancel path — handler may catch and wind down gracefully. */
        response_release_declared_length(response, ZSTR_LEN(chunk));
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

/* {{{ proto HttpResponse::tryWrite(string $chunk): bool
 *
 * Non-blocking write(). Returns false when the outbound queue has no room —
 * nothing was queued and no header was committed, so the same chunk can be
 * offered again later. A peer that is gone is NOT reported as false: it
 * throws HttpException 499, because "wait" and "stop" call for opposite
 * reactions and one bool cannot carry both.
 *
 * The refused chunk is a slice of one byte stream, so dropping it corrupts
 * the body — retry it or stop. Only the framed dialects (SSE events, gRPC
 * messages) carry droppable units.
 *
 * HTTP/1 neither refuses nor returns promptly: it keeps no queue of its own,
 * so the kernel socket buffer is the queue, and an accepted chunk waits on the
 * socket for as long as a blocking write() would. Its depth is not readable
 * portably, so the exception is documented rather than closed — the cost of a
 * chunk was measured instead, in dev/BENCHMARKS.md. */
ZEND_METHOD(TrueAsync_HttpResponse, tryWrite)
{
    zend_string *chunk;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_STR(chunk)
    ZEND_PARSE_PARAMETERS_END();

    http_response_object *response = Z_HTTP_RESPONSE_P(ZEND_THIS);

    if (response_check_stream_usable(response, "tryWrite", true)) {
        return;
    }

    /* Dead peer first: false must mean "full", and only that. */
    if (response->stream_ops->is_alive != NULL
        && !response->stream_ops->is_alive(response->stream_ctx)) {
        zend_throw_exception_ex(http_exception_ce, 499, "stream closed by peer");
        return;
    }

    /* Dropped and recorded, as in write(). */
    if (response->is_head) {
        response->head_streamed = true;
        RETURN_TRUE;
    }

    /* Same order as write(): the declaration is read, and an over-run refused,
     * while the response is still uncommitted. An over-run is not a full queue
     * — false would tell the handler to retry a chunk that can never fit. */
    if (response_check_declared_length(response, "tryWrite", ZSTR_LEN(chunk))) {
        return;
    }

    /* The commit precedes the append because the wrapper installed here is
     * what encodes the chunk, and the transport emits headers from inside.
     * No transport can refuse a first offer — each opens its queue on that
     * call and answers "room" while the queue is absent — so a refusal never
     * arrives with the response still uncommitted. A transport that ever
     * refuses a first offer would have to unwind the commit and the wrapper
     * together. */
    http_response_stream_commit_once(Z_OBJ_P(ZEND_THIS), response);

    zend_string_addref(chunk);
    const int rc = response->stream_ops->append_chunk(
        response->stream_ctx, chunk, true);

    /* append_chunk consumes the ref on every path, refusals included. */
    if (rc == HTTP_STREAM_APPEND_BACKPRESSURE) {
        /* Nothing was queued, so the bytes reserved above are free again — the
         * handler is expected to offer the same chunk once there is room. */
        response_release_declared_length(response, ZSTR_LEN(chunk));
        RETURN_FALSE;
    }

    /* The transport may already have thrown a more precise reason — an
     * over-sized chunk, say. 499 is the fallback diagnosis, not an override. */
    if (rc == HTTP_STREAM_APPEND_STREAM_DEAD) {
        response_release_declared_length(response, ZSTR_LEN(chunk));

        if (EXPECTED(EG(exception) == NULL)) {
            zend_throw_exception_ex(http_exception_ce, 499, "stream closed by peer");
        }

        return;
    }

    RETURN_TRUE;
}
/* }}} */

/* {{{ proto HttpResponse::awaitWritable(?int $timeoutMs = null): bool
 *
 * Wait until the outbound queue has room again, and report whether it has.
 * The companion to tryWrite(): that call says "not now", this one waits for
 * "now" instead of spinning.
 *
 * The wait belongs to the transport, which keeps its own deadline and re-pumps
 * its drain on each wake. A transport with no queue (HTTP/1) has nothing to
 * wait for and answers true at once. A transport that can be full but offers
 * no way to wait answers false rather than true — a caller told "go ahead"
 * would spin and never yield, which on a pool worker freezes every other
 * request on that thread. */
ZEND_METHOD(TrueAsync_HttpResponse, awaitWritable)
{
    zend_long timeout_ms      = 0;
    bool      timeout_is_null = true;

    ZEND_PARSE_PARAMETERS_START(0, 1)
        Z_PARAM_OPTIONAL
        Z_PARAM_LONG_OR_NULL(timeout_ms, timeout_is_null)
    ZEND_PARSE_PARAMETERS_END();

    if (UNEXPECTED(!timeout_is_null && timeout_ms < 0)) {
        zend_throw_exception(http_server_runtime_exception_ce,
            "awaitWritable(): timeout must not be negative", 0);
        return;
    }

    http_response_object *response = Z_HTTP_RESPONSE_P(ZEND_THIS);

    if (response_check_stream_usable(response, "awaitWritable", false)) {
        return;
    }

    const http_response_stream_ops_t *ops = response->stream_ops;

    /* No queue of its own, or room already: nothing to wait for. */
    if (ops->sendable == NULL || ops->sendable(response->stream_ctx)) {
        RETURN_TRUE;
    }

    if (ops->wait_writable == NULL) {
        RETURN_FALSE;
    }

    zend_coroutine_t *co = ZEND_ASYNC_CURRENT_COROUTINE;

    if (co == NULL || ZEND_ASYNC_IS_SCHEDULER_CONTEXT) {
        zend_throw_exception(http_server_runtime_exception_ce,
            "awaitWritable() needs a coroutine to suspend — call it from a handler", 0);
        return;
    }

    const bool woken = ops->wait_writable(response->stream_ctx,
        timeout_is_null ? 0u : (uint32_t)timeout_ms);

    /* A timeout or a cancellation arrives as the transport's exception; it is
     * left to propagate rather than flattened into false, which would hide a
     * cancelled request behind "still full". */
    if (EG(exception) != NULL) {
        return;
    }

    if (!woken) {
        RETURN_FALSE;
    }

    RETURN_BOOL(ops->sendable == NULL || ops->sendable(response->stream_ctx));
}
/* }}} */

/* {{{ proto HttpResponse::setGrpcEncoding(string $encoding): static
 * Declare the response message encoding (grpc-encoding header) before the
 * first writeMessage(). Mirrors grpc-java setCompression / C++
 * set_compression_algorithm: enabling compression is a per-call decision;
 * per-message the wire only allows *skipping* it. */
ZEND_METHOD(TrueAsync_HttpResponse, setGrpcEncoding)
{
    zend_string *encoding;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_STR(encoding)
    ZEND_PARSE_PARAMETERS_END();

    http_response_object *response = Z_HTTP_RESPONSE_P(ZEND_THIS);

    if (response->grpc_mode == GRPC_MODE_NONE) {
        zend_throw_exception(http_server_runtime_exception_ce,
            "setGrpcEncoding() is only available on gRPC responses", 0);
        return;
    }

    if (response->closed || response->streaming || response->headers_sent) {
        zend_throw_exception(http_server_runtime_exception_ce,
            "setGrpcEncoding() must be called before the first writeMessage()", 0);
        return;
    }

    if (zend_string_equals_literal_ci(encoding, "identity")) {
        response->grpc_compress = false;
        zend_hash_str_del(response->headers, "grpc-encoding",
                          sizeof("grpc-encoding") - 1);
        RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
    }

    if (!zend_string_equals_literal_ci(encoding, "gzip")) {
        zend_throw_exception_ex(http_server_runtime_exception_ce, 0,
            "Unsupported grpc-encoding \"%s\" (supported: gzip, identity)",
            ZSTR_VAL(encoding));
        return;
    }

#ifdef HAVE_HTTP_COMPRESSION
    response->grpc_compress = true;

    zval enc;
    ZVAL_STRING(&enc, "gzip");
    zend_hash_str_update(response->headers, "grpc-encoding",
                         sizeof("grpc-encoding") - 1, &enc);

    RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
#else
    zend_throw_exception(http_server_runtime_exception_ce,
        "gzip grpc-encoding requires the compression module", 0);
#endif
}
/* }}} */

/* A gRPC message is bytes of this response's own framing, so it answers to the
 * same guards as write(): closed, sealed by sendFile(), detached, already
 * framed as SSE, or holding a buffered body it would discard. @p method names
 * the caller in the exception text. Throws and answers false. */
static bool grpc_message_streamable(const http_response_object *response, const char *method)
{
    return !response_check_stream_usable(response, method, true);
}

/* Frame one message for the gRPC wire: compress it when an encoding was
 * declared, prepend the five-byte length prefix, and base64 the frame under
 * grpc-web-text. Returns a fresh reference, which append_chunk consumes. */
static zend_string *grpc_message_frame(http_response_object *response, zend_string *message)
{
    zend_string *payload = message;
    bool         gzipped = false;

#ifdef HAVE_HTTP_COMPRESSION
    if (response->grpc_compress) {
        zend_string *gz = grpc_message_deflate_gzip(
            ZSTR_VAL(message), ZSTR_LEN(message));

        /* deflate failure → identity for this message (compressed-flag 0),
         * which the spec permits under a declared encoding */
        if (gz != NULL) {
            payload = gz;
            gzipped = true;
        }
    }
#endif

    zend_string *framed = grpc_frame_message(
        ZSTR_VAL(payload), ZSTR_LEN(payload), gzipped);

    if (gzipped) {
        zend_string_release(payload);   /* framed copied it; drop the gz buffer */
    }

    /* grpc-web-text: each frame is base64-encoded independently */
    if (response->grpc_mode == GRPC_MODE_WEB_TEXT) {
        zend_string *const b64 =
            grpc_web_text_encode(ZSTR_VAL(framed), ZSTR_LEN(framed));

        zend_string_release(framed);
        framed = b64;
    }

    return framed;
}

/* The first message switches the response into streaming mode at the PHP
 * boundary; the protocol header commit happens inside append_chunk. */
static void grpc_message_begin_stream(http_response_object *response)
{
    if (!response->streaming) {
        response->streaming = true;
        response->committed = true;
        response->headers_sent = true;
    }
}

/* {{{ proto HttpResponse::writeMessage(string $message): static
 * Stream one gRPC length-prefixed message; first call commits, like write().
 * Compressed automatically when setGrpcEncoding('gzip') was declared. */
ZEND_METHOD(TrueAsync_HttpResponse, writeMessage)
{
    zend_string *message;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_STR(message)
    ZEND_PARSE_PARAMETERS_END();

    http_response_object *response = Z_HTTP_RESPONSE_P(ZEND_THIS);

    if (!grpc_message_streamable(response, "writeMessage")) {
        return;
    }

    /* Dropped and recorded, as in write(). */
    if (response->is_head) {
        response->head_streamed = true;
        RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
    }

    /* The framed message is this response's own body bytes, so a declared
     * length counts them as it counts a write(). The frame is built first
     * because its length is what reaches the wire, and the guard runs before
     * the commit for the reason write() has it there. */
    zend_string *frame = grpc_message_frame(response, message);

    if (response_check_declared_length(response, "writeMessage", ZSTR_LEN(frame))) {
        zend_string_release(frame);
        return;
    }

    grpc_message_begin_stream(response);

    const size_t frame_len = ZSTR_LEN(frame);
    const int    rc         = response->stream_ops->append_chunk(
        response->stream_ctx, frame, false);

    if (rc == HTTP_STREAM_APPEND_STREAM_DEAD) {
        response_release_declared_length(response, frame_len);
        zend_throw_exception_ex(http_exception_ce, 499,
            "stream closed by peer");
        return;
    }

    RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
}
/* }}} */

/* {{{ proto HttpResponse::tryWriteMessage(string $message): bool */
ZEND_METHOD(TrueAsync_HttpResponse, tryWriteMessage)
{
    zend_string *message;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_STR(message)
    ZEND_PARSE_PARAMETERS_END();

    http_response_object *response = Z_HTTP_RESPONSE_P(ZEND_THIS);

    if (!grpc_message_streamable(response, "tryWriteMessage")) {
        return;
    }

    /* Dead peer first, so false carries one meaning only — the queue is full. */
    if (response->stream_ops->is_alive != NULL
        && !response->stream_ops->is_alive(response->stream_ctx)) {
        zend_throw_exception_ex(http_exception_ce, 499, "stream closed by peer");
        return;
    }

    /* Dropped and recorded, as in write(). */
    if (response->is_head) {
        response->head_streamed = true;
        RETURN_TRUE;
    }

    zend_string *frame = grpc_message_frame(response, message);

    if (response_check_declared_length(response, "tryWriteMessage", ZSTR_LEN(frame))) {
        zend_string_release(frame);
        return;
    }

    grpc_message_begin_stream(response);

    const size_t frame_len = ZSTR_LEN(frame);
    const int    rc        = response->stream_ops->append_chunk(
        response->stream_ctx, frame, true);

    if (rc == HTTP_STREAM_APPEND_STREAM_DEAD) {
        response_release_declared_length(response, frame_len);

        if (EXPECTED(EG(exception) == NULL)) {
            zend_throw_exception_ex(http_exception_ce, 499, "stream closed by peer");
        }

        return;
    }

    if (rc == HTTP_STREAM_APPEND_BACKPRESSURE) {
        /* Nothing was queued: the offer can be made again, so the bytes are
         * not spent. */
        response_release_declared_length(response, frame_len);
        RETURN_FALSE;
    }

    RETURN_TRUE;
}
/* }}} */

/* {{{ proto HttpResponse::sendable(): bool
 *
 * Tombstone: the declaration outlives the method for one minor release,
 * because shipped adapter code calls it and its two replacements cannot be
 * guessed from the name. */
ZEND_METHOD(TrueAsync_HttpResponse, sendable)
{
    (void)return_value;
    ZEND_PARSE_PARAMETERS_NONE();

    zend_throw_exception(http_server_runtime_exception_ce,
        "sendable() is gone: it answered liveness and queue depth with one bool. "
        "Use isWritable() for liveness, tryWrite()/awaitWritable() for room", 0);
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

    /* What this rejects is a path relative to the current directory, which
     * the handler cannot reason about. A leading slash qualifies everywhere;
     * on Windows so do a drive letter and a UNC prefix, and refusing those
     * left every sendFile() there answering 500. */
    if (UNEXPECTED(!IS_ABSOLUTE_PATH(ZSTR_VAL(path), ZSTR_LEN(path))
                   && !IS_SLASH(ZSTR_VAL(path)[0]))) {
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

    if (opts->status != 0) {
        if (!response_status_is_final(opts->status, "sendFile()")) {
            http_send_file_request_free(req);
            return;
        }

        /* A file body under a status defined to carry none is the desync the
         * PHP path refuses: the client ends the message at the blank line and
         * reads the file's first bytes as the next status line. The static
         * head builder states Content-Length from the file size and knows
         * nothing about the rule, so the refusal belongs here. */
        if (!response_status_carries_body(opts->status)) {
            const int bad = opts->status;
            http_send_file_request_free(req);
            zend_throw_exception_ex(http_server_invalid_argument_exception_ce, 0,
                "sendFile(): status %d carries no body, and a file is one", bad);
            return;
        }
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
    http_compression_mark_no_compression(Z_OBJ_P(ZEND_THIS));
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

    if (response->aborted) {
        zend_throw_exception(http_server_runtime_exception_ce,
            "Response has already been aborted", 0);
        return;
    }

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
        /* Final data that would pass the declared length is refused like any
         * other write, and the stream is still finished below: the body is
         * short, so the finisher fails it, and the peer learns that from the
         * wire rather than from a connection that stops. The exception is the
         * handler's diagnosis and travels on top of a response already closed,
         * so an end() in a finally block gets the ordinary "already closed". */
        if (data != NULL && ZSTR_LEN(data) > 0
            && !response_check_declared_length(response, "end", ZSTR_LEN(data))) {
            zend_string_addref(data);

            if (response->stream_ops->append_chunk(response->stream_ctx, data, false)
                == HTTP_STREAM_APPEND_STREAM_DEAD) {
                /* Nothing left, so the count must not say it did — the
                 * finisher below reads it to decide whether the promise was
                 * kept. */
                response_release_declared_length(response, ZSTR_LEN(data));
            }
        }

        (void)http_response_finish_stream(Z_OBJ_P(ZEND_THIS), false, -1);
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

/* {{{ proto HttpResponse::abort(): void
 *
 * Finish a started stream as failed, so the peer can tell a body that stopped
 * from a body that finished. What that costs is protocol-specific and is the
 * whole point: HTTP/1 loses the connection, because chunked framing has no
 * other way to say it.
 *
 * $errorCode is the reset code of whichever protocol carries the response, and
 * it does not travel between them: HTTP/2 and HTTP/3 number the same
 * conditions differently, and HTTP/1 has no field for one. Omitted, each
 * transport uses its own INTERNAL_ERROR — which is what a handler that does
 * not know its protocol wants.
 *
 * How much there is to disown decides the rest. A response that never started
 * streaming — a buffered one, or a HEAD, whose write() drops every chunk — is
 * left alone, and the handler's own exception goes on to become the status. A
 * stream started with nothing yet on the wire is finished cleanly instead: the
 * peer gets the empty response the transport commits for it, which tells it
 * more than a stream that merely stops.
 *
 * Neither of those is an error. The natural call site is a catch or finally
 * block, and a method that throws from there replaces the diagnosis the
 * handler was carrying; for the same reason a second abort() is a no-op rather
 * than a complaint.
 *
 * end() already on the wire is different: the client has been told the body is
 * whole, and no later call can take that back. That one throws. */
ZEND_METHOD(TrueAsync_HttpResponse, abort)
{
    zend_long error_code  = 0;
    bool      code_is_null = true;

    ZEND_PARSE_PARAMETERS_START(0, 1)
        Z_PARAM_OPTIONAL
        Z_PARAM_LONG_OR_NULL(error_code, code_is_null)
    ZEND_PARSE_PARAMETERS_END();

    /* Both registries keep their codes in a 32-bit field, so anything wider is
     * a mistake rather than a code this server has no name for. */
    if (!code_is_null && (error_code < 0 || error_code > 0xFFFFFFFFLL)) {
        zend_throw_exception(http_server_invalid_argument_exception_ce,
            "abort(): error code must be between 0 and 4294967295", 0);
        return;
    }

    http_response_object *response = Z_HTTP_RESPONSE_P(ZEND_THIS);

    if (response->aborted) {
        return;
    }

    if (response->closed) {
        zend_throw_exception(http_server_runtime_exception_ce,
            "Response has already been closed — the body was already finished cleanly", 0);
        return;
    }

    (void)http_response_finish_stream(Z_OBJ_P(ZEND_THIS), true,
                                      code_is_null ? -1 : (int64_t) error_code);
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

/* {{{ proto HttpResponse::isWritable(): bool
 *
 * True while output is still possible: end() was not called, the response is
 * not sealed by sendFile(), and the peer has not gone. A false answer is
 * final, unlike the queue depth tryWrite() reports — so a streaming loop
 * stops on !isWritable() and yields on a refused tryWrite(). */
ZEND_METHOD(TrueAsync_HttpResponse, isWritable)
{
    ZEND_PARSE_PARAMETERS_NONE();

    http_response_object *response = Z_HTTP_RESPONSE_P(ZEND_THIS);

    if (response->closed
        || response->send_file_req != NULL
        || response->stream_ops == NULL) {
        RETURN_FALSE;
    }

    /* No liveness op — the response state above is all we know. */
    if (response->stream_ops->is_alive == NULL) {
        RETURN_TRUE;
    }

    RETURN_BOOL(response->stream_ops->is_alive(response->stream_ctx));
}
/* }}} */

/* {{{ proto HttpResponse::isEnded(): bool
 *
 * Reports the response, not the connection: a peer that has gone leaves this
 * false until the handler finishes the response, whichever way it finishes it —
 * end() or abort(). isWritable() answers liveness. */
ZEND_METHOD(TrueAsync_HttpResponse, isEnded)
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
    response->aborted = false;
    response->wire_failed = false;
    response->committed = false;
    response->streaming = false;
    response->grpc_mode = 0;
    response->stream_ops = NULL;
    response->stream_ctx = NULL;
    response->declared_length = -1;
    response->written_length = 0;
    response->transport_body_size = -1;
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
    http_compression_state_free(obj);
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

void http_response_set_grpc_mode(zend_object *obj, const uint8_t mode)
{
    http_response_from_obj(obj)->grpc_mode = mode;
}

uint8_t http_response_get_grpc_mode(zend_object *obj)
{
    return http_response_from_obj(obj)->grpc_mode;
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
