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
#ifdef PHP_WIN32
# include <ws2tcpip.h>   /* inet_ntop */
#else
# include <arpa/inet.h>  /* inet_ntop, ntohs */
#endif
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

    /* Repeat-call caches. getHeaders() rebuilds a PHP array (and, for a
     * reactor-built persistent request, deep-copies every string) on every
     * call; getBody() deep-copies a persistent body on every call. Both
     * results are immutable once req->complete, so the first post-complete
     * call is cached here. CoW protects the array from user mutation. */
    zval            headers_cache;   /* IS_UNDEF until cached */
    zend_string    *body_cache;      /* NULL until cached (persistent body only) */

    zend_object     std;
} http_request_object;

/* Get HttpRequest object from zval */
static inline http_request_object* http_request_from_obj(zend_object *obj)
{
    return (http_request_object*)((char*)(obj) - offsetof(http_request_object, std));
}

#define Z_HTTP_REQUEST_P(zv) http_request_from_obj(Z_OBJ_P(zv))

http_request_t *http_request_from_zobj(zend_object *obj)
{
    return http_request_from_obj(obj)->request;
}

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
    ZVAL_UNDEF(&intern->headers_cache);
    intern->body_cache = NULL;

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

    zval_ptr_dtor(&intern->headers_cache);

    if (intern->body_cache != NULL) {
        zend_string_release(intern->body_cache);
        intern->body_cache = NULL;
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

    if (!Z_ISUNDEF(intern->headers_cache)) {
        RETURN_COPY(&intern->headers_cache);
    }

    http_request_retval_ht(return_value, intern->request->headers);

    /* Headers are final once the request is complete (trailers included) —
     * cache the built array so repeat calls stop re-copying. */
    if (intern->request->complete && Z_TYPE_P(return_value) == IS_ARRAY) {
        ZVAL_COPY(&intern->headers_cache, return_value);
    }
}

ZEND_METHOD(TrueAsync_HttpRequest, getBody)
{
    http_request_object *intern = Z_HTTP_REQUEST_P(ZEND_THIS);
    ZEND_PARSE_PARAMETERS_NONE();

    if (intern->body_cache != NULL) {
        RETURN_STR_COPY(intern->body_cache);
    }

    if (!intern->request->body) {
        RETURN_EMPTY_STRING();
    }

    http_request_retval_str(return_value, intern->request->body);

    /* Only the persistent (reactor-built) body pays a deep copy per call —
     * cache that one; the ZMM path is already a refcounted addref. */
    if (intern->request->complete
        && (GC_FLAGS(intern->request->body) & IS_STR_PERSISTENT)
        && Z_TYPE_P(return_value) == IS_STRING) {
        intern->body_cache = zend_string_copy(Z_STR_P(return_value));
    }
}

/* {{{ proto HttpRequest::readMessage(): ?string
 * Next gRPC message from the request body; null when none remain. */
ZEND_METHOD(TrueAsync_HttpRequest, readMessage)
{
    http_request_object *intern = Z_HTTP_REQUEST_P(ZEND_THIS);
    ZEND_PARSE_PARAMETERS_NONE();

    http_request_t *req = intern->request;

    if (req == NULL) {
        RETURN_NULL();
    }

    zend_string *msg = NULL;
    bool         compressed = false;
    int          rc;

    /* grpc-web-text: decode the base64 body once, deframe from the result */
    const bool web_text = (req->grpc_mode == GRPC_MODE_WEB_TEXT);

    if (web_text && req->grpc_text_body == NULL) {
        if (req->body == NULL) {
            RETURN_NULL();
        }

        req->grpc_text_body =
            grpc_web_text_decode(ZSTR_VAL(req->body), ZSTR_LEN(req->body));

        if (req->grpc_text_body == NULL) {
            zend_throw_exception(http_server_runtime_exception_ce,
                "malformed base64 in grpc-web-text request body", 0);
            RETURN_NULL();
        }
    }

    if (!web_text && !req->body_streaming && req->body_upgrade_to_stream != NULL) {
        req->body_upgrade_to_stream(req);
    }

    if (web_text) {
        rc = grpc_deframe_next(ZSTR_VAL(req->grpc_text_body),
                               ZSTR_LEN(req->grpc_text_body),
                               &req->grpc_read_offset,
                               GRPC_MAX_RECV_MESSAGE, &compressed, &msg);
    } else if (req->body_streaming) {
        /* Full-duplex: accumulate chunks into grpc_reassembly, suspend
         * between them. Drop the consumed prefix only once it dominates
         * the buffer — compacting on every call re-moves the whole tail
         * per extracted message, O(messages × backlog); the half-buffer
         * threshold keeps each byte moved at most once (amortized) while
         * still bounding the buffer at ~2× live data. */
        if (req->grpc_read_offset > 0 && req->grpc_reassembly.s != NULL) {
            const size_t total  = ZSTR_LEN(req->grpc_reassembly.s);
            const size_t remain = req->grpc_read_offset < total
                                 ? total - req->grpc_read_offset : 0;

            if (remain == 0) {
                ZSTR_LEN(req->grpc_reassembly.s) = 0;
                ZSTR_VAL(req->grpc_reassembly.s)[0] = '\0';
                req->grpc_read_offset = 0;
            } else if (req->grpc_read_offset >= remain) {
                memmove(ZSTR_VAL(req->grpc_reassembly.s),
                        ZSTR_VAL(req->grpc_reassembly.s) + req->grpc_read_offset,
                        remain);
                ZSTR_LEN(req->grpc_reassembly.s) = remain;
                ZSTR_VAL(req->grpc_reassembly.s)[remain] = '\0';
                req->grpc_read_offset = 0;
            }
        }

        for (;;) {
            const char  *buf = req->grpc_reassembly.s
                             ? ZSTR_VAL(req->grpc_reassembly.s) : "";
            const size_t len = req->grpc_reassembly.s
                             ? ZSTR_LEN(req->grpc_reassembly.s) : 0;

            rc = grpc_deframe_next(buf, len, &req->grpc_read_offset,
                                   GRPC_MAX_RECV_MESSAGE, &compressed, &msg);

            if (rc != 0) {
                break;   /* 1 = message extracted, -1 = protocol error */
            }

            /* Incomplete frame — pull the next body chunk, or wait / EOF. */
            zend_string *chunk = http_body_stream_pop(req);

            if (chunk != NULL) {
                smart_str_append(&req->grpc_reassembly, chunk);
                zend_string_release(chunk);
                continue;
            }

            if (req->body_eof) {
                if (UNEXPECTED(req->body_error)) {
                    zend_throw_exception(http_server_runtime_exception_ce,
                        "gRPC request body stream error", 0);
                    RETURN_THROWS();
                }
                RETURN_NULL();   /* client half-closed, no more messages */
            }

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
    } else {
        /* Buffered: deframe from the fully-received body via the cursor. */
        if (req->body == NULL) {
            RETURN_NULL();
        }
        rc = grpc_deframe_next(ZSTR_VAL(req->body), ZSTR_LEN(req->body),
                               &req->grpc_read_offset,
                               GRPC_MAX_RECV_MESSAGE, &compressed, &msg);
    }

    if (rc < 0) {
        zend_throw_exception(http_server_runtime_exception_ce,
            "gRPC message length exceeds the maximum allowed size", 0);
        RETURN_NULL();
    }

    if (rc == 0) {
        /* No complete message left at the cursor. */
        RETURN_NULL();
    }

    if (compressed) {
#ifdef HAVE_HTTP_COMPRESSION
        /* Per-message compression: decode per grpc-encoding (gzip). */
        zend_string *inflated = NULL;

        if (grpc_message_inflate(req, ZSTR_VAL(msg), ZSTR_LEN(msg),
                                 &inflated) == 0) {
            zend_string_release(msg);
            RETURN_STR(inflated);
        }
#endif
        zend_string_release(msg);
        zend_throw_exception(http_server_runtime_exception_ce,
            "unsupported or invalid gRPC message compression", 0);
        RETURN_NULL();
    }

    RETURN_STR(msg);
}

/* {{{ proto HttpRequest::getGrpcTimeout(): ?float
 * grpc-timeout header in seconds, or null. The server does not enforce it. */
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

size_t http_sockaddr_ip(const struct sockaddr *addr, socklen_t addr_len,
                        char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) {
        return 0;
    }

    out[0] = '\0';

    if (addr == NULL) {
        return 0;
    }

    const void *src = NULL;
    int         af  = 0;

    if (addr->sa_family == AF_INET
        && addr_len >= (socklen_t)sizeof(struct sockaddr_in)) {
        af  = AF_INET;
        src = &((const struct sockaddr_in *)addr)->sin_addr;
    } else if (addr->sa_family == AF_INET6
               && addr_len >= (socklen_t)sizeof(struct sockaddr_in6)) {
        af  = AF_INET6;
        src = &((const struct sockaddr_in6 *)addr)->sin6_addr;
    } else {
        return 0;   /* AF_UNIX and friends have no IP */
    }

    if (inet_ntop(af, src, out, (socklen_t)out_len) == NULL) {
        out[0] = '\0';
        return 0;
    }

    return strlen(out);
}

uint16_t http_sockaddr_port(const struct sockaddr *addr, socklen_t addr_len)
{
    if (addr == NULL) {
        return 0;
    }

    if (addr->sa_family == AF_INET
        && addr_len >= (socklen_t)sizeof(struct sockaddr_in)) {
        return ntohs(((const struct sockaddr_in *)addr)->sin_port);
    }

    if (addr->sa_family == AF_INET6
        && addr_len >= (socklen_t)sizeof(struct sockaddr_in6)) {
        return ntohs(((const struct sockaddr_in6 *)addr)->sin6_port);
    }

    return 0;
}

/* Build the logger's access record from a completed request. Lives here, not in
 * src/log: turning a request into a log record is the request layer's job, and
 * it is what keeps the logging layer free of protocol types.
 *
 * `ip_buf` backs rec->client_address — the record borrows, so the buffer must
 * outlive the emit (callers keep it on the same stack frame). */
void http_request_fill_access_rec(const http_request_t *req,
                                  zend_object *response_obj,
                                  http_access_rec_t *rec,
                                  char *ip_buf, size_t ip_buf_len)
{
    memset(rec, 0, sizeof *rec);

    if (req == NULL || response_obj == NULL) {
        return;
    }

    rec->method   = req->method != NULL ? ZSTR_VAL(req->method) : NULL;
    rec->url_path = req->path != NULL ? ZSTR_VAL(req->path)
                  : (req->uri != NULL ? ZSTR_VAL(req->uri) : NULL);

    /* url.query is the raw string after '?' — req->uri holds path?query, and
     * req->path is the path alone, so the split is already done for us. */
    if (req->uri != NULL) {
        const char *q = strchr(ZSTR_VAL(req->uri), '?');
        rec->url_query = (q != NULL && q[1] != '\0') ? q + 1 : NULL;
    }

    rec->status        = http_response_get_status(response_obj);
    rec->response_size = http_response_get_body_len(response_obj);

    /* network.protocol.version per OTel: the bare version, not a scheme alias
     * ("2", never "h2"). Note "2"/"3" have no minor — OTel spells them so. */
    rec->protocol_version = req->http_major >= 3 ? "3"
                          : req->http_major == 2 ? "2"
                          : req->http_minor == 0 ? "1.0" : "1.1";

    /* Duration from the telemetry stamps (start_logging forces them on when an
     * access sink exists). end_ns may not be stamped yet this far down the
     * completion path, so fall back to a fresh reading. */
    if (req->start_ns != 0) {
        const uint64_t end = req->end_ns != 0 ? req->end_ns : zend_hrtime();

        if (end > req->start_ns) {
            rec->duration_ns = end - req->start_ns;
        }
    }

    if (req->peer_len > 0 && ip_buf != NULL && ip_buf_len > 0) {
        if (http_sockaddr_ip((const struct sockaddr *)&req->peer, req->peer_len,
                             ip_buf, ip_buf_len) > 0) {
            rec->client_address = ip_buf;
        }

        rec->client_port = http_sockaddr_port((const struct sockaddr *)&req->peer,
                                              req->peer_len);
    }

    if (req->has_trace) {
        rec->trace_id    = req->trace_id;
        rec->span_id     = req->span_id;
        rec->trace_flags = req->trace_flags;
    }
}

ZEND_METHOD(TrueAsync_HttpRequest, getRemoteAddress)
{
    http_request_object *intern = Z_HTTP_REQUEST_P(ZEND_THIS);
    ZEND_PARSE_PARAMETERS_NONE();

    char   ip[INET6_ADDRSTRLEN];
    size_t n = http_sockaddr_ip((const struct sockaddr *)&intern->request->peer,
                                intern->request->peer_len, ip, sizeof ip);

    if (n == 0) {
        RETURN_NULL();   /* Unix-socket listener — no IP peer */
    }

    RETURN_STRINGL(ip, n);
}

ZEND_METHOD(TrueAsync_HttpRequest, getRemotePort)
{
    http_request_object *intern = Z_HTTP_REQUEST_P(ZEND_THIS);
    ZEND_PARSE_PARAMETERS_NONE();

    const uint16_t port = http_sockaddr_port(
        (const struct sockaddr *)&intern->request->peer,
        intern->request->peer_len);

    if (port == 0) {
        RETURN_NULL();
    }

    RETURN_LONG((zend_long)port);
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

/* Store one request header, combining duplicate names per RFC 9110 §5.3:
 * values append in arrival order, "cookie" joins with "; " (HPACK/QPACK
 * crumb reassembly, RFC 9113 §8.2.3 / RFC 9114 §4.2.1), everything else
 * with ", ". Borrows `name`; takes ownership of `value`. */
void http_request_store_header(http_request_t *req, zend_string *name,
                               zend_string *value)
{
    zval *const existing = zend_hash_find(req->headers, name);

    if (existing == NULL) {
        zval zv;
        ZVAL_STR(&zv, value);
        zend_hash_add_new(req->headers, name, &zv);
        return;
    }

    const char *const sep =
        zend_string_equals_literal(name, "cookie") ? "; " : ", ";
    zend_string *const old = Z_STR_P(existing);
    const size_t len = ZSTR_LEN(old) + 2 + ZSTR_LEN(value);
    zend_string *const combined = zend_string_alloc(len, req->persistent);

    memcpy(ZSTR_VAL(combined), ZSTR_VAL(old), ZSTR_LEN(old));
    memcpy(ZSTR_VAL(combined) + ZSTR_LEN(old), sep, 2);
    memcpy(ZSTR_VAL(combined) + ZSTR_LEN(old) + 2,
           ZSTR_VAL(value), ZSTR_LEN(value));
    ZSTR_VAL(combined)[len] = '\0';

    zend_string_release(value);
    zend_string_release(old);
    ZVAL_STR(existing, combined);
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
