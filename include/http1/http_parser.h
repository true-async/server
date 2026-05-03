/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

#ifndef HTTP_PARSER_H
#define HTTP_PARSER_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include "../deps/llhttp/llhttp.h"

#include "php.h"
#include "zend_smart_str.h"
#include "Zend/zend_async_API.h"

/* Forward declaration (full type in http_connection.h) */
typedef struct _http_connection_t http_connection_t;

/* Parser-level error codes recorded on http1_parser_t::parse_error so
 * the connection layer can build an RFC-compliant 4xx response.
 * HTTP_PARSE_OK is the steady state; any non-OK value means
 * http_parser_execute returned -1 and the parser is unusable for this
 * connection. */
typedef enum {
    HTTP_PARSE_OK = 0,
    HTTP_PARSE_ERR_URI_TOO_LONG,             /* 414 */
    HTTP_PARSE_ERR_HEADER_NAME_TOO_LARGE,    /* 431 */
    HTTP_PARSE_ERR_HEADER_VALUE_TOO_LARGE,   /* 431 */
    HTTP_PARSE_ERR_HEADERS_TOTAL_TOO_LARGE,  /* 431 */
    HTTP_PARSE_ERR_TOO_MANY_HEADERS,         /* 431 — header count cap */
    HTTP_PARSE_ERR_BAD_HEADER,               /* 400 — value without name */
    HTTP_PARSE_ERR_BODY_TOO_LARGE,           /* 413 */
    HTTP_PARSE_ERR_MALFORMED,                /* 400 — llhttp generic */
    HTTP_PARSE_ERR_INVALID_CONTENT_LENGTH,   /* 400 — non-numeric/overflow/sign in CL */
    HTTP_PARSE_ERR_CONFLICTING_HEADERS,      /* 400 — CL+TE coexist or duplicate CL with differing values (RFC 9112 §6.3) */
    HTTP_PARSE_ERR_INVALID_HTTP_VERSION,     /* 400 — version other than HTTP/1.0 or HTTP/1.1 */
    HTTP_PARSE_ERR_OUT_OF_MEMORY,            /* 503 — emalloc bailout in parser callback */
    HTTP_PARSE_ERR_SERVICE_UNAVAILABLE       /* 503 — admission reject (overload shedding) */
} http_parse_error_t;

/*
 * HTTP/1.1 Parser (llhttp-backed) and unified logical-request struct.
 *
 *   - http_request_t  — per logical request (method/uri/headers/body +
 *                       multipart state). Lifetime may outlive the parser:
 *                       HTTP/2+ can multiplex, and bodies can stream after
 *                       dispatch.
 *   - http1_parser_t  — per HTTP/1 connection framing parser (llhttp +
 *                       smart_str builders).
 */

/* Maximum limits for DDoS protection */
#define HTTP_MAX_URI_SIZE        (8 * 1024)      /* 8 KB */
#define HTTP_MAX_HEADER_NAME     (1 * 1024)      /* 1 KB */
#define HTTP_MAX_HEADER_VALUE    (8 * 1024)      /* 8 KB */
#define HTTP_MAX_HEADERS_TOTAL   (64 * 1024)     /* 64 KB */
#define HTTP_MAX_HEADER_COUNT    256             /* Per-request cap on number of header fields (DoS) */
#define HTTP_DEFAULT_BODY_BUFFER (8 * 1024)      /* 8 KB for chunked */
#define HTTP_HEADERS_INITIAL_SIZE 16             /* Initial HashTable size */

/* Forward declaration for multipart processor */
struct mp_processor_t;

/* Parsed HTTP request (logical-request struct).
 *
 * Field order: 8-byte pointers first, then size_t, then 1-byte fields
 * clustered at the end. Multipart state lives on the request, not the
 * parser, so it belongs to the request that produced it.
 */
typedef struct http_request_t http_request_t;
struct http_request_t {
    /* Request line */
    zend_string *method;
    zend_string *uri;

    /* Headers (HashTable with lowercase keys -> zend_string values) */
    HashTable   *headers;

    /* Body */
    zend_string *body;

    /* Body-progress event.
     * Lazily created by awaitBody() on the first suspend; notified by
     * the parser on body_complete once the event-loop read path lands.
     * Currently stays NULL because dispatch still happens at
     * message-complete, so handlers never need to wait. */
    zend_async_event_t *body_event;

    /* Coroutine running the user handler for THIS request. Set in
     * http_connection_dispatch_request right after ENQUEUE; cleared
     * at the first statement of http_handler_coroutine_dispose. Lets
     * a parser-error path that fires AFTER dispatch (e.g. streamed
     * body exceeding limit) cancel the in-flight handler with an
     * HttpException via ZEND_ASYNC_CANCEL. Lives on the request, not
     * the connection, because one TCP carries many requests
     * (keep-alive today, HTTP/2 stream multiplex tomorrow — each
     * stream gets its own request and its own handler coroutine). */
    zend_coroutine_t   *coroutine;

    /* Multipart state (owned by the request) */
    struct mp_processor_t *multipart_proc;  /* Multipart processor (NULL if not multipart) */
    HashTable   *post_data;                 /* Parsed POST fields */
    HashTable   *files;                     /* Parsed uploaded files */

    /* 8-byte counters */
    size_t       content_length;

    /* Per-request timing (ns from zend_hrtime). Lives on the request
     * because one TCP connection hosts many of them (keep-alive,
     * HTTP/2 streams) — storing on the connection would overwrite
     * between in-flight requests.
     *
     *   enqueue_ns:  parser finished the request, dispatched for handling.
     *   start_ns:    handler coroutine actually began executing.
     *   end_ns:      handler returned / response was sent.
     *
     * Derived intervals fed to telemetry and CoDel:
     *   sojourn  = start_ns  − enqueue_ns   (reactor scheduling latency,
     *                                        CoDel's control signal)
     *   service  = end_ns    − start_ns     (handler work time)
     *   lifetime = end_ns    − enqueue_ns   (full request wall time) */
    uint64_t     enqueue_ns;
    uint64_t     start_ns;
    uint64_t     end_ns;

    /* Shared-ownership refcount (guards against post-dispatch UAF).
     * Allocators start at 1. Holders that need to outlive the original
     * owner (HTTP/2 + HTTP/3 streams that keep writing body bytes
     * post-dispatch while the PHP HttpRequest object also owns a ref)
     * call http_request_addref. Each holder releases via
     * http_request_destroy; last release actually frees.
     *
     * H1 doesn't need addref — dispatch fires at message_complete, by
     * which time the body is already finalized and the parser has no
     * more writes to perform on the request. Initial 1 = the original
     * single owner; release brings it to 0 and frees, same observable
     * behavior as the pre-refcount API. */
    unsigned     refcount;

    /* Custom release callback. NULL = legacy efree(req) at refcount=0.
     * Non-NULL: invoked instead of efree, used by stream owners that
     * embed http_request_t as their first field and want the slot to
     * return to a slab pool rather than being efree'd individually.
     *
     * The callback receives the same pointer that http_request_destroy
     * decremented to 0 (i.e. the embedding struct's address — first-
     * field cast trick). It is responsible for any extra teardown the
     * embedder needs (smart_str_free / zval_ptr_dtor / etc.) and then
     * returns the slot to its pool. The base http_request_destroy has
     * already cleaned up every field declared in http_request_t. */
    void       (*release)(struct http_request_t *req);

    /* W3C Trace Context. Populated by http_request_parse_trace_context
     * at on_headers_complete iff server has telemetry enabled and the
     * request carried a valid traceparent. trace_id == 16 zero bytes
     * means "absent". traceparent_raw / tracestate_raw retain the raw
     * header strings for HttpRequest::getTraceparent etc. */
    uint8_t      trace_id[16];
    uint8_t      span_id[8];
    uint8_t      trace_flags;
    bool         has_trace;
    zend_string *traceparent_raw;
    zend_string *tracestate_raw;

    /* 1-byte fields clustered */
    uint8_t      http_major;
    uint8_t      http_minor;
    bool         chunked;
    bool         keep_alive;
    bool         complete;
    bool         use_multipart;
};

/* HTTP/1 parser.
 *
 * Field order: llhttp embedded structs, then request pointer, then
 * smart_str builders (pointer + size_t), then size_t counters, then
 * bool flags clustered at the end.
 */
/* Dispatch callback type. Fired from on_headers_complete exactly once
 * per request, as soon as method/uri/headers are ready. The callee
 * takes ownership of `req` — the parser will keep writing into it via
 * on_body / on_message_complete but will not free it on its own. */
typedef void (*http_dispatch_cb_t)(http_connection_t *conn, http_request_t *req);

typedef struct http1_parser_t {
#ifdef HAVE_LLHTTP
    llhttp_t          parser;
    llhttp_settings_t settings;
#endif

    http_request_t   *request;

    /* Dispatch wiring. Set by http_parser_attach() when the connection
     * first binds the parser; cleared by http_parser_reset_for_reuse()
     * when the parser returns to the pool. on_headers_complete calls
     * dispatch_cb(conn, request) once per message. */
    http_connection_t *conn;
    http_dispatch_cb_t dispatch_cb;

    /* Smart string builders for accumulating chunks (each 16 bytes) */
    smart_str         uri_builder;
    smart_str         body_builder;
    smart_str         header_name_builder;
    smart_str         header_value_builder;

    /* 8-byte counters */
    size_t            body_offset;           /* Offset for pre-allocated body */
    size_t            total_headers_size;    /* Total size of all headers (DDoS) */
    size_t            max_body_size;         /* Configurable body size limit */

    /* Smuggling-defense state (RFC 9112 §6.3). Tracked across all
     * save_current_header calls within one request so on_headers_complete
     * can reject conflicting/duplicate CL or CL+TE coexistence. Reset in
     * on_message_begin alongside the rest of per-request state. */
    uint64_t          cl_value_first;        /* First parsed Content-Length value (only valid when cl_seen_count > 0) */
    uint16_t          cl_seen_count;         /* Number of Content-Length headers seen */
    uint16_t          header_count;          /* Number of header fields parsed (DoS cap) */
    bool              te_chunked_seen;       /* Transfer-Encoding: chunked seen */

    /* Boolean flags (clustered to avoid interior padding) */
    bool              in_header_value;       /* Currently parsing header value? */
    bool              paused;                /* llhttp is paused after on_message_complete —
                                              * next execute must call llhttp_resume first */
    /* Last parser-level error. HTTP_PARSE_OK after a clean execute or
     * fresh reset; any other value means execute returned -1 and the
     * connection layer should call http_connection_emit_parse_error
     * before disposing. Reset by http_parser_reset / reset_for_reuse. */
    http_parse_error_t parse_error;

    bool              owns_request;          /* True while `request` is still parser-owned.
                                              * Cleared the moment we hand the request over
                                              * to dispatch_cb (on_headers_complete); cleanup
                                              * paths only destroy the request if this is true.
                                              * Independent of the req's own state — crucial
                                              * because after handoff `request` may dangle
                                              * (handler may have already freed it). */
} http1_parser_t;

/* Parser API */

/* Initialize parser with configurable body size limit */
http1_parser_t* http_parser_create(size_t max_body_size);

/* Attach connection + dispatch callback to a just-acquired parser.
 * Must be called before the first http_parser_execute. The parser
 * fires dispatch_cb(conn, request) from on_headers_complete. */
void http_parser_attach(http1_parser_t *parser,
                        http_connection_t *conn,
                        http_dispatch_cb_t dispatch_cb);

/* Parse data.
 *
 * consumed_out (optional): number of bytes from `data` that llhttp actually
 * consumed. When on_message_complete fires, llhttp is paused and consumed
 * may be less than len — the tail belongs to the next pipelined request
 * and the caller must preserve it. A subsequent call to http_parser_execute
 * transparently resumes llhttp. */
int http_parser_execute(http1_parser_t *parser, const char *data, size_t len, size_t *consumed_out);

/* Get parsed request (after complete) */
http_request_t* http_parser_get_request(const http1_parser_t *parser);

/* Check if request is complete */
static inline bool http_parser_is_complete(const http1_parser_t *parser)
{
    return parser->request && parser->request->complete;
}

/* Reset for next request */
void http_parser_reset(http1_parser_t *parser);

/* Reset for reuse with buffer recycling (based on refcount) */
void http_parser_reset_for_reuse(http1_parser_t *parser);

/* Destroy parser */
void http_parser_destroy(http1_parser_t *parser);

/* Mapping helpers for parse_error → HTTP status / reason. The reason
 * string is a single-line static literal suitable both as the status
 * line text and as the response body. */
int         http_parse_error_to_status(http_parse_error_t err);
const char *http_parse_error_reason(http_parse_error_t err);

/* Bump request refcount. Used by H2/H3 stream layers right at dispatch
 * time so the stream can keep writing body bytes / fire body_event
 * post-dispatch while the PHP HttpRequest object independently owns
 * its own ref. NULL-safe. */
void http_request_addref(http_request_t *req);

/* Release a request reference. Decrements refcount; when it hits 0
 * the request is actually freed. Pre-refcount the function name was
 * literally "destroy" — we kept the name because every call site's
 * intent is still "I'm done with this; let it go" and refcount=1 is
 * the canonical single-owner case where release == destroy. NULL-safe. */
void http_request_destroy(http_request_t *req);

#endif /* HTTP_PARSER_H */
