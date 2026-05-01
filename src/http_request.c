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
#include "log/trace_context.h"

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
    return (http_request_object*)((char*)(obj) - XtOffsetOf(http_request_object, std));
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
    /* This constructor is private - instances are created internally */
    ZEND_PARSE_PARAMETERS_NONE();
}

ZEND_METHOD(TrueAsync_HttpRequest, getMethod)
{
    http_request_object *intern = Z_HTTP_REQUEST_P(ZEND_THIS);
    ZEND_PARSE_PARAMETERS_NONE();

    if (!intern->request->method) {
        RETURN_EMPTY_STRING();
    }

    RETURN_STR_COPY(intern->request->method);
}

ZEND_METHOD(TrueAsync_HttpRequest, getUri)
{
    http_request_object *intern = Z_HTTP_REQUEST_P(ZEND_THIS);
    ZEND_PARSE_PARAMETERS_NONE();

    if (!intern->request->uri) {
        RETURN_EMPTY_STRING();
    }

    RETURN_STR_COPY(intern->request->uri);
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
        RETURN_STR_COPY(Z_STR_P(value));
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
        RETURN_STR_COPY(Z_STR_P(value));
    }

    RETURN_EMPTY_STRING();
}

ZEND_METHOD(TrueAsync_HttpRequest, getHeaders)
{
    http_request_object *intern = Z_HTTP_REQUEST_P(ZEND_THIS);
    ZEND_PARSE_PARAMETERS_NONE();

    ZVAL_ARR(return_value, zend_array_dup(intern->request->headers));
}

ZEND_METHOD(TrueAsync_HttpRequest, getBody)
{
    http_request_object *intern = Z_HTTP_REQUEST_P(ZEND_THIS);
    ZEND_PARSE_PARAMETERS_NONE();

    if (!intern->request->body) {
        RETURN_EMPTY_STRING();
    }

    RETURN_STR_COPY(intern->request->body);
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
        ZVAL_ARR(return_value, zend_array_dup(intern->request->post_data));
    } else {
        array_init(return_value);
    }
}

ZEND_METHOD(TrueAsync_HttpRequest, getFiles)
{
    http_request_object *intern = Z_HTTP_REQUEST_P(ZEND_THIS);
    ZEND_PARSE_PARAMETERS_NONE();

    if (intern->request->files) {
        ZVAL_ARR(return_value, zend_array_dup(intern->request->files));
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

ZEND_METHOD(TrueAsync_HttpRequest, getContentType)
{
    http_request_object *intern = Z_HTTP_REQUEST_P(ZEND_THIS);
    ZEND_PARSE_PARAMETERS_NONE();

    zend_string *name = zend_string_init("content-type", sizeof("content-type") - 1, 0);
    zval *value = zend_hash_find(intern->request->headers, name);
    zend_string_release(name);

    if (value && Z_TYPE_P(value) == IS_STRING) {
        RETURN_STR_COPY(Z_STR_P(value));
    }

    RETURN_NULL();
}

ZEND_METHOD(TrueAsync_HttpRequest, getContentLength)
{
    http_request_object *intern = Z_HTTP_REQUEST_P(ZEND_THIS);
    ZEND_PARSE_PARAMETERS_NONE();

    zend_string *name = zend_string_init("content-length", sizeof("content-length") - 1, 0);
    zval *value = zend_hash_find(intern->request->headers, name);
    zend_string_release(name);

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
    RETURN_STR_COPY(intern->request->traceparent_raw);
}

ZEND_METHOD(TrueAsync_HttpRequest, getTraceState)
{
    http_request_object *intern = Z_HTTP_REQUEST_P(ZEND_THIS);
    ZEND_PARSE_PARAMETERS_NONE();

    if (intern->request->tracestate_raw == NULL) {
        RETURN_NULL();
    }
    RETURN_STR_COPY(intern->request->tracestate_raw);
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
    http_request_object_handlers.offset = XtOffsetOf(http_request_object, std);
    http_request_object_handlers.free_obj = http_request_free_object;
    http_request_object_handlers.clone_obj = NULL;  /* No cloning */
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
