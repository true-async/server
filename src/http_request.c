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
#include "php_http_server.h"
#include "http1/http_parser.h"
#include "http_body_stream.h"
#include "grpc/grpc.h"
#include "core/async_plain_event.h"
#include "log/trace_context.h"
#include "Zend/zend_async_API.h"
#include "Zend/zend_exceptions.h"
#include "main/php_variables.h"

#include <errno.h>
#include <stdlib.h>
#ifndef PHP_WIN32
# include <strings.h>
#endif

/* Include generated arginfo */
#include "../stubs/HttpRequest.php_arginfo.h"

/* HttpRequest class entry */
zend_class_entry *http_request_ce;

/* HttpRequest object structure.
 *
 * Owns a http_request_t* directly. The parser is NOT held — it stays
 * with the connection so on_body / on_message_complete can still write
 * into the request after dispatch (streaming mode).
 */
typedef struct {
    http_request_t *request;
    zend_object     std;
} http_request_object;

/* Get HttpRequest object from zval */
static inline http_request_object* http_request_from_obj(zend_object *obj)
{
    return (http_request_object*)((char*)(obj) - offsetof(http_request_object, std));
}

#define Z_HTTP_REQUEST_P(zv) http_request_from_obj(Z_OBJ_P(zv))

/* Object handlers */
static zend_object_handlers http_request_object_handlers;

/* Create HttpRequest object */
static zend_object* http_request_create_object(zend_class_entry *ce)
{
    http_request_object *intern = zend_object_alloc(sizeof(http_request_object), ce);

    zend_object_std_init(&intern->std, ce);
    object_properties_init(&intern->std, ce);

    intern->std.handlers = &http_request_object_handlers;
    intern->request = NULL;

    return &intern->std;
}

/* Free HttpRequest object - destroy the owned request struct.
 *
 * The parser is not our concern — it stays on the connection and is
 * returned to the pool in http_connection_destroy. */
static void http_request_free_object(zend_object *object)
{
    http_request_object *intern = http_request_from_obj(object);

    if (intern->request) {
        http_request_destroy(intern->request);
        intern->request = NULL;
    }

    zend_object_std_dtor(&intern->std);
}

/* Methods implementation */

/* Private constructor - prevents direct instantiation */
ZEND_METHOD(TrueAsync_HttpRequest, __construct)
{
    (void)return_value;
    /* This constructor is private - instances are created internally */
    ZEND_PARSE_PARAMETERS_NONE();
}

/* Hand a request string to PHP in the worker's ZMM domain. A reactor-built
 * (persistent malloc) string must be deep-copied — the engine would otherwise
 * efree malloc memory. ZMM / interned strings are returned by addref as before.
 * The domain is read off the string itself (self-describing), so mixed-domain
 * requests (persistent method/uri/headers + ZMM path/query) are handled per
 * field. */
static void http_request_retval_str(zval *out, zend_string *str)
{
    if (UNEXPECTED(GC_FLAGS(str) & IS_STR_PERSISTENT)) {
        ZVAL_STRINGL(out, ZSTR_VAL(str), ZSTR_LEN(str));
    } else {
        ZVAL_STR_COPY(out, str);
    }
}

/* Hand a request HashTable to PHP in the worker's ZMM domain. ZMM tables are
 * dup'd (cheap, refcounted) as before; a persistent (reactor-built) table is
 * rebuilt with ZMM copies — zend_array_dup would addref persistent keys/values
 * that the engine then efree's (heap corruption). Request tables hold string
 * values only. */
static void http_request_retval_ht(zval *out, HashTable *ht)
{
    if (EXPECTED(!(GC_FLAGS(ht) & IS_ARRAY_PERSISTENT))) {
        ZVAL_ARR(out, zend_array_dup(ht));
        return;
    }

    zend_array *dst;
    ALLOC_HASHTABLE(dst);
    zend_hash_init(dst, zend_hash_num_elements(ht), NULL, ZVAL_PTR_DTOR, 0);

    zend_string *key;
    zend_ulong   idx;
    zval        *val;
    ZEND_HASH_FOREACH_KEY_VAL(ht, idx, key, val) {
        zval copy;

        if (EXPECTED(Z_TYPE_P(val) == IS_STRING)) {
            ZVAL_STRINGL(&copy, Z_STRVAL_P(val), Z_STRLEN_P(val));
        } else {
            ZVAL_COPY(&copy, val);
        }

        if (key != NULL) {
            zend_string *const key_copy = zend_string_init(ZSTR_VAL(key), ZSTR_LEN(key), 0);
            zend_hash_update(dst, key_copy, &copy);
            zend_string_release(key_copy);
        } else {
            zend_hash_index_update(dst, idx, &copy);
        }
    } ZEND_HASH_FOREACH_END();

    ZVAL_ARR(out, dst);
}

ZEND_METHOD(TrueAsync_HttpRequest, getMethod)
{
    http_request_object *intern = Z_HTTP_REQUEST_P(ZEND_THIS);
    ZEND_PARSE_PARAMETERS_NONE();

    if (!intern->request->method) {
        RETURN_EMPTY_STRING();
    }

    http_request_retval_str(return_value, intern->request->method);
}

ZEND_METHOD(TrueAsync_HttpRequest, getUri)
{
    http_request_object *intern = Z_HTTP_REQUEST_P(ZEND_THIS);
    ZEND_PARSE_PARAMETERS_NONE();

    if (!intern->request->uri) {
        RETURN_EMPTY_STRING();
    }

    http_request_retval_str(return_value, intern->request->uri);
}

ZEND_METHOD(TrueAsync_HttpRequest, getHttpVersion)
{
    http_request_object *intern = Z_HTTP_REQUEST_P(ZEND_THIS);
    char version[4];
    ZEND_PARSE_PARAMETERS_NONE();

    /* HTTP/1.x major/minor are single digits — skip libc format_converter. */
    version[0] = '0' + (char)intern->request->http_major;
    version[1] = '.';
    version[2] = '0' + (char)intern->request->http_minor;
    version[3] = '\0';

    RETURN_STRINGL(version, 3);
}

ZEND_METHOD(TrueAsync_HttpRequest, hasHeader)
{
    http_request_object *intern = Z_HTTP_REQUEST_P(ZEND_THIS);
    zend_string *name = NULL;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_STR(name)
    ZEND_PARSE_PARAMETERS_END();

    zend_string *name_lower = zend_string_tolower(name);
    bool exists = zend_hash_exists(intern->request->headers, name_lower);
    zend_string_release(name_lower);

    RETURN_BOOL(exists);
}

ZEND_METHOD(TrueAsync_HttpRequest, getHeader)
{
    http_request_object *intern = Z_HTTP_REQUEST_P(ZEND_THIS);
    zend_string *name = NULL;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_STR(name)
    ZEND_PARSE_PARAMETERS_END();

    zend_string *name_lower = zend_string_tolower(name);
    zval *value = zend_hash_find(intern->request->headers, name_lower);
    zend_string_release(name_lower);

    if (value && Z_TYPE_P(value) == IS_STRING) {
        http_request_retval_str(return_value, Z_STR_P(value));
        return;
    }

    RETURN_NULL();
}

ZEND_METHOD(TrueAsync_HttpRequest, getHeaderLine)
{
    http_request_object *intern = Z_HTTP_REQUEST_P(ZEND_THIS);
    zend_string *name = NULL;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_STR(name)
    ZEND_PARSE_PARAMETERS_END();

    zend_string *name_lower = zend_string_tolower(name);
    zval *value = zend_hash_find(intern->request->headers, name_lower);
    zend_string_release(name_lower);

    /* In our implementation headers are stored as single strings */
    if (value && Z_TYPE_P(value) == IS_STRING) {
        http_request_retval_str(return_value, Z_STR_P(value));
        return;
    }

    RETURN_EMPTY_STRING();
}

ZEND_METHOD(TrueAsync_HttpRequest, getHeaders)
{
    http_request_object *intern = Z_HTTP_REQUEST_P(ZEND_THIS);
    ZEND_PARSE_PARAMETERS_NONE();

    http_request_retval_ht(return_value, intern->request->headers);
}

ZEND_METHOD(TrueAsync_HttpRequest, getBody)
{
    http_request_object *intern = Z_HTTP_REQUEST_P(ZEND_THIS);
    ZEND_PARSE_PARAMETERS_NONE();

    if (!intern->request->body) {
        RETURN_EMPTY_STRING();
    }

    http_request_retval_str(return_value, intern->request->body);
}

/* {{{ proto HttpRequest::readMessage(): ?string
 *
 * Deframe and return the next gRPC message from the request body (one
 * 5-byte-length-prefixed frame), advancing an internal cursor. Returns
 * null once no complete message remains — call once for a unary RPC, loop
 * for client-streaming. The returned bytes are the raw (protobuf) message;
 * decoding stays in PHP userland. */
ZEND_METHOD(TrueAsync_HttpRequest, readMessage)
{
    http_request_object *intern = Z_HTTP_REQUEST_P(ZEND_THIS);
    ZEND_PARSE_PARAMETERS_NONE();

    http_request_t *req = intern->request;

    if (req == NULL || req->body == NULL) {
        RETURN_NULL();
    }

    zend_string *msg = NULL;
    const int rc = grpc_deframe_next(ZSTR_VAL(req->body), ZSTR_LEN(req->body),
                                     &req->grpc_read_offset,
                                     GRPC_MAX_RECV_MESSAGE, NULL, &msg);

    if (rc < 0) {
        zend_throw_exception(http_server_runtime_exception_ce,
            "gRPC message length exceeds the maximum allowed size", 0);
        RETURN_NULL();
    }

    if (rc == 0) {
        /* No complete message left at the cursor. */
        RETURN_NULL();
    }

    RETURN_STR(msg);
}

/* {{{ proto HttpRequest::getGrpcTimeout(): ?float
 *
 * The gRPC call deadline parsed from the `grpc-timeout` request header, in
 * seconds (fractional), or null when the client sent none / it was
 * malformed. The server does not itself abort the handler when the deadline
 * elapses (the client enforces its own deadline); a handler can read this
 * and bound its own work against it. */
ZEND_METHOD(TrueAsync_HttpRequest, getGrpcTimeout)
{
    http_request_object *intern = Z_HTTP_REQUEST_P(ZEND_THIS);
    ZEND_PARSE_PARAMETERS_NONE();

    const uint64_t ns = grpc_parse_timeout_ns(intern->request);

    if (ns == 0) {
        RETURN_NULL();
    }

    RETURN_DOUBLE((double)ns / 1000000000.0);
}

ZEND_METHOD(TrueAsync_HttpRequest, hasBody)
{
    http_request_object *intern = Z_HTTP_REQUEST_P(ZEND_THIS);
    ZEND_PARSE_PARAMETERS_NONE();

    RETURN_BOOL(intern->request->body && ZSTR_LEN(intern->request->body) > 0);
}

ZEND_METHOD(TrueAsync_HttpRequest, isKeepAlive)
{
    http_request_object *intern = Z_HTTP_REQUEST_P(ZEND_THIS);
    ZEND_PARSE_PARAMETERS_NONE();

    RETURN_BOOL(intern->request->keep_alive);
}

ZEND_METHOD(TrueAsync_HttpRequest, getPost)
{
    http_request_object *intern = Z_HTTP_REQUEST_P(ZEND_THIS);
    ZEND_PARSE_PARAMETERS_NONE();

    if (intern->request->post_data) {
        http_request_retval_ht(return_value, intern->request->post_data);
    } else {
        array_init(return_value);
    }
}

ZEND_METHOD(TrueAsync_HttpRequest, getFiles)
{
    http_request_object *intern = Z_HTTP_REQUEST_P(ZEND_THIS);
    ZEND_PARSE_PARAMETERS_NONE();

    if (intern->request->files) {
        http_request_retval_ht(return_value, intern->request->files);
    } else {
        array_init(return_value);
    }
}

ZEND_METHOD(TrueAsync_HttpRequest, getFile)
{
    http_request_object *intern = Z_HTTP_REQUEST_P(ZEND_THIS);
    zend_string *name = NULL;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_STR(name)
    ZEND_PARSE_PARAMETERS_END();

    if (!intern->request->files) {
        RETURN_NULL();
    }

    zval *file = zend_hash_find(intern->request->files, name);

    if (!file) {
        RETURN_NULL();
    }

    /* If it's an array (multiple files), return first one */
    if (Z_TYPE_P(file) == IS_ARRAY) {
        zval *first = zend_hash_index_find(Z_ARRVAL_P(file), 0);

        if (first && Z_TYPE_P(first) == IS_OBJECT) {
            RETURN_OBJ_COPY(Z_OBJ_P(first));
        }

        RETURN_NULL();
    }

    /* Single file */
    if (Z_TYPE_P(file) == IS_OBJECT) {
        RETURN_OBJ_COPY(Z_OBJ_P(file));
    }

    RETURN_NULL();
}

/* Parse URI into path + query_params on first access; results are cached. */
static void http_request_ensure_uri_parsed(http_request_t *req)
{
    if (req->path != NULL) {
        return;
    }

    if (!req->uri || ZSTR_LEN(req->uri) == 0) {
        req->path         = ZSTR_EMPTY_ALLOC();
        req->query_params = zend_new_array(0);
        return;
    }

    const char *uri  = ZSTR_VAL(req->uri);
    size_t      ulen = ZSTR_LEN(req->uri);
    const char *q    = memchr(uri, '?', ulen);

    if (!q) {
        /* path is a worker-derived (ZMM) field. Aliasing a persistent uri
         * by addref would make path persistent; deep-copy in that case so
         * getPath can addref it to PHP. ZMM uri stays a cheap shared ref. */
        req->path         = (GC_FLAGS(req->uri) & IS_STR_PERSISTENT)
                                ? zend_string_init(ZSTR_VAL(req->uri), ZSTR_LEN(req->uri), 0)
                                : zend_string_copy(req->uri);
        req->query_params = zend_new_array(0);
        return;
    }

    req->path = zend_string_init(uri, (size_t)(q - uri), 0);

    zval arr;
    array_init(&arr);
    size_t qslen = ulen - (size_t)(q + 1 - uri);

    if (qslen > 0) {
        /* php_default_treat_data takes ownership of the string (calls efree),
         * handles percent-decoding, '+'-as-space, PHP array notation, and
         * max_input_vars — identical to how PHP populates $_GET. */
        php_default_treat_data(PARSE_STRING, estrndup(q + 1, qslen), &arr);
    }

    req->query_params = Z_ARR(arr);
}

ZEND_METHOD(TrueAsync_HttpRequest, getPath)
{
    http_request_object *intern = Z_HTTP_REQUEST_P(ZEND_THIS);
    ZEND_PARSE_PARAMETERS_NONE();

    http_request_ensure_uri_parsed(intern->request);
    RETURN_STR_COPY(intern->request->path);
}

ZEND_METHOD(TrueAsync_HttpRequest, getQuery)
{
    http_request_object *intern = Z_HTTP_REQUEST_P(ZEND_THIS);
    ZEND_PARSE_PARAMETERS_NONE();

    http_request_ensure_uri_parsed(intern->request);
    http_request_retval_ht(return_value, intern->request->query_params);
}

ZEND_METHOD(TrueAsync_HttpRequest, getQueryParam)
{
    http_request_object *intern = Z_HTTP_REQUEST_P(ZEND_THIS);
    zend_string *name;
    zval        *default_value = NULL;

    ZEND_PARSE_PARAMETERS_START(1, 2)
        Z_PARAM_STR(name)
        Z_PARAM_OPTIONAL
        Z_PARAM_ZVAL(default_value)
    ZEND_PARSE_PARAMETERS_END();

    http_request_ensure_uri_parsed(intern->request);

    zval *val = zend_hash_find(intern->request->query_params, name);

    if (val) {
        RETURN_COPY(val);
    }

    if (default_value) {
        RETURN_COPY(default_value);
    }

    RETURN_NULL();
}

ZEND_METHOD(TrueAsync_HttpRequest, getContentType)
{
    http_request_object *intern = Z_HTTP_REQUEST_P(ZEND_THIS);
    ZEND_PARSE_PARAMETERS_NONE();

    zval *value = zend_hash_str_find(intern->request->headers,
                                     "content-type", sizeof("content-type") - 1);

    if (value && Z_TYPE_P(value) == IS_STRING) {
        RETURN_STR_COPY(Z_STR_P(value));
    }

    RETURN_NULL();
}

ZEND_METHOD(TrueAsync_HttpRequest, getContentLength)
{
    http_request_object *intern = Z_HTTP_REQUEST_P(ZEND_THIS);
    ZEND_PARSE_PARAMETERS_NONE();

    zval *value = zend_hash_str_find(intern->request->headers,
                                     "content-length", sizeof("content-length") - 1);

    if (value && Z_TYPE_P(value) == IS_STRING) {
        char *end = NULL;
        errno = 0;
        long long len = strtoll(Z_STRVAL_P(value), &end, 10);

        if (errno == 0 && end != Z_STRVAL_P(value) && len >= 0) {
            RETURN_LONG((zend_long)len);
        }
    }

    RETURN_NULL();
}

ZEND_METHOD(TrueAsync_HttpRequest, getTraceParent)
{
    http_request_object *intern = Z_HTTP_REQUEST_P(ZEND_THIS);
    ZEND_PARSE_PARAMETERS_NONE();

    if (intern->request->traceparent_raw == NULL) {
        RETURN_NULL();
    }

    http_request_retval_str(return_value, intern->request->traceparent_raw);
}

ZEND_METHOD(TrueAsync_HttpRequest, getTraceState)
{
    http_request_object *intern = Z_HTTP_REQUEST_P(ZEND_THIS);
    ZEND_PARSE_PARAMETERS_NONE();

    if (intern->request->tracestate_raw == NULL) {
        RETURN_NULL();
    }

    http_request_retval_str(return_value, intern->request->tracestate_raw);
}

ZEND_METHOD(TrueAsync_HttpRequest, getTraceId)
{
    http_request_object *intern = Z_HTTP_REQUEST_P(ZEND_THIS);
    ZEND_PARSE_PARAMETERS_NONE();

    if (!intern->request->has_trace) {
        RETURN_NULL();
    }

    char hex[33];
    trace_hex_encode(intern->request->trace_id, 16, hex);
    RETURN_STRINGL(hex, 32);
}

ZEND_METHOD(TrueAsync_HttpRequest, getSpanId)
{
    http_request_object *intern = Z_HTTP_REQUEST_P(ZEND_THIS);
    ZEND_PARSE_PARAMETERS_NONE();

    if (!intern->request->has_trace) {
        RETURN_NULL();
    }

    char hex[17];
    trace_hex_encode(intern->request->span_id, 8, hex);
    RETURN_STRINGL(hex, 16);
}

ZEND_METHOD(TrueAsync_HttpRequest, getTraceFlags)
{
    http_request_object *intern = Z_HTTP_REQUEST_P(ZEND_THIS);
    ZEND_PARSE_PARAMETERS_NONE();

    if (!intern->request->has_trace) {
        RETURN_NULL();
    }

    RETURN_LONG((zend_long)intern->request->trace_flags);
}

ZEND_METHOD(TrueAsync_HttpRequest, awaitBody)
{
    http_request_object *intern = Z_HTTP_REQUEST_P(ZEND_THIS);
    ZEND_PARSE_PARAMETERS_NONE();

    http_request_t *req = intern->request;

    /* Fast path: body has already been fully received. This is the
     * default state because dispatch currently runs at message-complete;
     * streaming mode (dispatch-at-headers-complete) is the one that
     * actually needs the suspend path below. */
    if (req == NULL || req->complete) {
        RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
    }

    /* Lazy-create the body_event trigger. Once installed, the parser's
     * on_message_complete hook fires it, which resolves all attached
     * wakers. */
    if (req->body_event == NULL) {
        zend_async_trigger_event_t *trig = ZEND_ASYNC_NEW_TRIGGER_EVENT();

        if (trig == NULL) {
            /* Out of memory / reactor shutdown — fall back to returning
             * $this as if the wait had completed. */
            RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
        }

        req->body_event = &trig->base;
    }

    /* Attach a waker to body_event and suspend. Mirrors the pattern
     * used by http_connection's async_io_req_await. */
    zend_coroutine_t *coroutine = ZEND_ASYNC_CURRENT_COROUTINE;

    if (ZEND_ASYNC_WAKER_NEW(coroutine) == NULL) {
        return;
    }

    zend_async_resume_when(coroutine, req->body_event, false,
                           zend_async_waker_callback_resolve, NULL);

    ZEND_ASYNC_SUSPEND();
    zend_async_waker_clean(coroutine);

    if (EG(exception) != NULL) {
        return;
    }

    RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
}

/* HttpRequest::readBody(int $maxLen = 65536): ?string
 *
 * Streaming body read (issue #26). Pulls the next chunk from the
 * per-request queue produced by H1/H2 parsers when
 * HttpServerConfig::setBodyStreamingEnabled(true) was set at server
 * start. Returns a non-empty string per chunk, or null at EOF.
 *
 * Each call returns exactly one parser-supplied chunk: an H2 DATA
 * frame payload (peer-side SETTINGS_MAX_FRAME_SIZE, default 16 KiB)
 * or one llhttp on_body slice (bounded by the H1 socket read,
 * DEFAULT_READ_BUFFER_SIZE = 8 KiB). TODO(issue #26): honour $maxLen
 * by coalescing consecutive queued chunks up to that cap so userland
 * can amortise the readBody() call overhead. Ignored today — kept in
 * the signature so the future change is binary-compatible.
 *
 * Throws \Exception if the body stream errored (peer reset, size cap
 * exceeded). Returns null idempotently at EOF.
 */
ZEND_METHOD(TrueAsync_HttpRequest, readBody)
{
    zend_long max_len = 65536;

    ZEND_PARSE_PARAMETERS_START(0, 1)
        Z_PARAM_OPTIONAL
        Z_PARAM_LONG(max_len)
    ZEND_PARSE_PARAMETERS_END();

    (void)max_len;  /* TODO(issue #26): wire into a pop-side coalesce. */

    http_request_object *intern = Z_HTTP_REQUEST_P(ZEND_THIS);
    http_request_t *req = intern->request;

    if (req == NULL) {
        RETURN_NULL();
    }

    /* ─── Case 0/1: body already fully buffered. Return whole, then EOF. */
    if (!req->body_streaming && req->complete) {
        if (req->body != NULL && ZSTR_LEN(req->body) > 0) {
            zend_string *body = req->body;
            req->body = NULL;
            RETURN_STR(body);
        }

        RETURN_NULL();
    }

    /* ─── Case 1 (still arriving, small body): just wait for completion
     * and return the whole accumulated body. No streaming machinery —
     * no body_data_event, no queue, no per-chunk overhead. The Case
     * triggers when the parser saw a Content-Length below
     * HTTP_BODY_STREAM_THRESHOLD and therefore left body_streaming=false
     * AND did NOT install body_upgrade_to_stream. */
    if (!req->body_streaming && req->body_upgrade_to_stream == NULL) {
        if (req->body_event == NULL) {
            zend_async_trigger_event_t *trig = ZEND_ASYNC_NEW_TRIGGER_EVENT();

            if (trig == NULL) {
                RETURN_NULL();
            }

            req->body_event = &trig->base;
        }

        zend_coroutine_t *co = ZEND_ASYNC_CURRENT_COROUTINE;

        if (co == NULL || ZEND_ASYNC_IS_SCHEDULER_CONTEXT) {
            RETURN_NULL();
        }

        if (ZEND_ASYNC_WAKER_NEW(co) == NULL) {
            return;
        }

        zend_async_resume_when(co, req->body_event, false,
                               zend_async_waker_callback_resolve, NULL);

        ZEND_ASYNC_SUSPEND();
        zend_async_waker_clean(co);

        if (EG(exception) != NULL) {
            return;
        }

        /* Body is now complete — drain req->body once. */
        if (req->body != NULL && ZSTR_LEN(req->body) > 0) {
            zend_string *body = req->body;
            req->body = NULL;
            RETURN_STR(body);
        }

        RETURN_NULL();
    }

    /* ─── Case 2: middle band (SMALL <= CL < AUTO). Parser was buffering
     * the body into the transport accumulator; upgrade NOW so this and
     * future on_data callbacks push into the queue. */
    if (!req->body_streaming && req->body_upgrade_to_stream != NULL) {
        req->body_upgrade_to_stream(req);
        /* body_streaming is now true; fall through to the streaming
         * pop/park loop below. */
    }

    /* ─── Case 3: streaming — pop from queue, park on body_data_event. */
    for (;;) {
        zend_string *chunk = http_body_stream_pop(req);

        if (chunk != NULL) {
            RETURN_STR(chunk);
        }

        if (req->body_eof) {
            if (UNEXPECTED(req->body_error)) {
                zend_throw_exception(zend_ce_exception,
                                     "Request body stream error", 0);
                RETURN_THROWS();
            }

            RETURN_NULL();
        }

        /* Park on body_data_event. Lazy-create if no chunk has been
         * pushed yet (early-call before parser delivered anything).
         * Plain in-thread event — producer (parser callback) runs on
         * the same reactor thread, so the uv_async_t indirection is
         * pure waste. */
        if (req->body_data_event == NULL) {
            req->body_data_event = async_plain_event_new();

            if (req->body_data_event == NULL) {
                RETURN_NULL();
            }
        }

        zend_coroutine_t *co = ZEND_ASYNC_CURRENT_COROUTINE;

        if (co == NULL || ZEND_ASYNC_IS_SCHEDULER_CONTEXT) {
            RETURN_NULL();
        }

        if (ZEND_ASYNC_WAKER_NEW(co) == NULL) {
            return;
        }

        zend_async_resume_when(co, req->body_data_event, false,
                               zend_async_waker_callback_resolve, NULL);

        ZEND_ASYNC_SUSPEND();
        zend_async_waker_clean(co);

        if (EG(exception) != NULL) {
            return;
        }
    }
}

/* Register HttpRequest class. Class entry + method table + FINAL flag
 * come from the auto-generated register_class_TrueAsync_HttpRequest()
 * in the arginfo header (the canonical modern-PHP-ext path). We layer
 * on the things the stub generator can't express: object handlers,
 * NO_DYNAMIC_PROPERTIES, and create_object. */
void http_request_class_register(void)
{
    http_request_ce = register_class_TrueAsync_HttpRequest();
    http_request_ce->ce_flags |= ZEND_ACC_NO_DYNAMIC_PROPERTIES;
    http_request_ce->create_object = http_request_create_object;

    memcpy(&http_request_object_handlers, zend_get_std_object_handlers(), sizeof(zend_object_handlers));
    http_request_object_handlers.offset = offsetof(http_request_object, std);
    http_request_object_handlers.free_obj = http_request_free_object;
    http_request_object_handlers.clone_obj = NULL;  /* No cloning */
}

/* Value dtor for a persistent (reactor-built) headers HashTable. Header
 * values are always zend_strings; zend_string_release is flag-aware
 * (pefree for IS_STR_PERSISTENT), unlike ZVAL_PTR_DTOR which routes
 * IS_STRING through zend_string_destroy == efree and would corrupt the
 * malloc heap on a persistent string. */
static void http_request_persistent_header_dtor(zval *zv)
{
    zend_string_release(Z_STR_P(zv));
}

void http_request_init_headers(http_request_t *req)
{
    if (req->headers != NULL) {
        return;
    }

    if (req->persistent) {
        req->headers = pemalloc(sizeof(HashTable), 1);
        zend_hash_init(req->headers, HTTP_HEADERS_INITIAL_SIZE, NULL,
                       http_request_persistent_header_dtor, 1);
        return;
    }

    ALLOC_HASHTABLE(req->headers);
    zend_hash_init(req->headers, HTTP_HEADERS_INITIAL_SIZE, NULL, ZVAL_PTR_DTOR, 0);
}

/* Helper: Create HttpRequest object wrapping an already-parsed
 * http_request_t. The object takes ownership; on free_obj it will
 * call http_request_destroy(req). */
zval* http_request_create_from_parsed(http_request_t *req)
{
    zval *obj = emalloc(sizeof(zval));

    object_init_ex(obj, http_request_ce);
    http_request_object *intern = Z_HTTP_REQUEST_P(obj);
    intern->request = req;

    return obj;
}

/* Method tokens are case-sensitive per RFC 7230 §3.1.1. */
bool http_request_method_is_get(const http_request_t *req)
{
    return req != NULL && req->method != NULL &&
           ZSTR_LEN(req->method) == 3 &&
           memcmp(ZSTR_VAL(req->method), "GET", 3) == 0;
}

bool http_request_method_is_head(const http_request_t *req)
{
    return req != NULL && req->method != NULL &&
           ZSTR_LEN(req->method) == 4 &&
           memcmp(ZSTR_VAL(req->method), "HEAD", 4) == 0;
}

/* Header lookup. Headers are stored with lowercase keys; caller must
 * pass an already-lowercased name. Multi-value headers can be stored
 * either as a single comma-joined string (HTTP/1) or as an array of
 * strings (HTTP/2 stream events) — return the first occurrence. */
const zend_string *http_request_find_header(const http_request_t *req,
                                            const char *name, size_t name_len)
{
    if (req == NULL || req->headers == NULL) {
        return NULL;
    }

    const zval *zv = zend_hash_str_find(req->headers, name, name_len);

    if (zv == NULL) {
        return NULL;
    }

    if (Z_TYPE_P(zv) == IS_STRING) {
        return Z_STR_P(zv);
    }

    if (Z_TYPE_P(zv) == IS_ARRAY) {
        const zval *first = zend_hash_index_find(Z_ARRVAL_P(zv), 0);

        if (first != NULL && Z_TYPE_P(first) == IS_STRING) {
            return Z_STR_P(first);
        }
    }

    return NULL;
}
