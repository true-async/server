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
#include "formats/multipart_processor.h"
#include "smart_str_scalable.h"
#include "http_known_strings.h"
#include "core/http_connection.h"   /* for http_connection_t::server layout */
#include "log/trace_context.h"

#include <string.h>
#ifndef PHP_WIN32
# include <strings.h>
#endif

static void save_current_header(http1_parser_t *parser);
static char* extract_boundary(const char *content_type);
static void finalize_multipart(http1_parser_t *parser);

/* Strict Content-Length parser per RFC 9110 §8.6 (Content-Length = 1*DIGIT)
 * and RFC 9112 §6.3. Rejects:
 *   - empty input
 *   - leading/trailing whitespace
 *   - sign characters (+/-) — strtoul silently treats "-1" as 2^64-1
 *   - any non-digit byte (so "100abc" or "100 200" no longer slip through)
 *   - arithmetic overflow past UINT64_MAX
 * Returns 0 on success and writes parsed value to *out, -1 on any
 * malformed value. */
static int parse_content_length(const char *s, size_t len, uint64_t *out)
{
    if (UNEXPECTED(len == 0)) return -1;
    uint64_t v = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (UNEXPECTED(c < '0' || c > '9')) return -1;
        unsigned d = (unsigned)(c - '0');
        if (UNEXPECTED(v > (UINT64_MAX - d) / 10)) return -1;
        v = v * 10 + d;
    }
    *out = v;
    return 0;
}

/* External: Create UploadedFile object from file info */
extern zval* uploaded_file_create_from_info(mp_file_info_t *info);

/* Helper: Check if string buffer can be reused (only we own it) */
static inline bool can_reuse_string_buffer(zend_string *str)
{
    /* Can reuse if: string exists, not interned, and refcount == 1 */
    return str && !ZSTR_IS_INTERNED(str) && GC_REFCOUNT(str) == 1;
}

/* Helper: Extract boundary from Content-Type header */
static char* extract_boundary(const char *content_type)
{
    const char *boundary_start = strstr(content_type, "boundary=");
    if (!boundary_start) {
        return NULL;
    }

    boundary_start += 9;  /* Skip "boundary=" */

    /* Skip optional quotes */
    if (*boundary_start == '"') {
        boundary_start++;
        const char *boundary_end = strchr(boundary_start, '"');
        if (!boundary_end) {
            return NULL;
        }
        size_t len = boundary_end - boundary_start;
        char *boundary = emalloc(len + 1);
        memcpy(boundary, boundary_start, len);
        boundary[len] = '\0';
        return boundary;
    }

    /* No quotes - find end (semicolon, space, or end of string) */
    const char *boundary_end = boundary_start;
    while (*boundary_end && *boundary_end != ';' && *boundary_end != ' ' && *boundary_end != '\t') {
        boundary_end++;
    }

    size_t len = boundary_end - boundary_start;
    if (len == 0) {
        return NULL;
    }

    char *boundary = emalloc(len + 1);
    memcpy(boundary, boundary_start, len);
    boundary[len] = '\0';
    return boundary;
}

/* Helper: Finalize multipart processing and create PHP arrays */
static void finalize_multipart(http1_parser_t *parser)
{
    http_request_t *req = parser->request;
    if (!req || !req->multipart_proc) {
        return;
    }

    mp_processor_t *proc = req->multipart_proc;

    /* Create POST data HashTable */
    ALLOC_HASHTABLE(req->post_data);
    zend_hash_init(req->post_data, 8, NULL, ZVAL_PTR_DTOR, 0);

    /* Get fields from processor */
    size_t field_count;
    mp_field_info_t *fields = mp_processor_get_fields(proc, &field_count);

    for (size_t i = 0; i < field_count; i++) {
        mp_field_info_t *field = &fields[i];
        if (!field->name) continue;

        zval zv;
        ZVAL_STRINGL(&zv, field->value, field->value_len);

        /* TODO: Handle PHP array notation (field[], field[key]) */
        /* For now, simple key=value storage */
        zend_hash_str_update(req->post_data, field->name, strlen(field->name), &zv);
    }

    /* Create Files HashTable */
    ALLOC_HASHTABLE(req->files);
    zend_hash_init(req->files, 8, NULL, ZVAL_PTR_DTOR, 0);

    /* Get files from processor */
    size_t file_count;
    mp_file_info_t *mp_files = mp_processor_get_files(proc, &file_count);

    for (size_t i = 0; i < file_count; i++) {
        mp_file_info_t *file = &mp_files[i];
        if (!file->field_name) continue;

        /* Create UploadedFile object */
        zval *file_obj = uploaded_file_create_from_info(file);

        /* Check if field name ends with [] (array notation) */
        size_t name_len = strlen(file->field_name);
        bool is_array = (name_len >= 2 &&
                        file->field_name[name_len - 2] == '[' &&
                        file->field_name[name_len - 1] == ']');

        if (is_array) {
            /* Strip [] from name */
            char *clean_name = estrndup(file->field_name, name_len - 2);

            /* Find or create array */
            zval *existing = zend_hash_str_find(req->files, clean_name, name_len - 2);
            if (existing && Z_TYPE_P(existing) == IS_ARRAY) {
                /* Add to existing array */
                zend_hash_next_index_insert(Z_ARRVAL_P(existing), file_obj);
                efree(file_obj);
            } else {
                /* Create new array */
                zval arr;
                array_init(&arr);
                zend_hash_next_index_insert(Z_ARRVAL(arr), file_obj);
                efree(file_obj);
                zend_hash_str_update(req->files, clean_name, name_len - 2, &arr);
            }
            efree(clean_name);
        } else {
            /* Single file */
            zend_hash_str_update(req->files, file->field_name, name_len, file_obj);
            efree(file_obj);
        }
    }
}

/* llhttp callbacks */

static int on_message_begin(llhttp_t* llhttp_parser)
{
    http1_parser_t *parser = (http1_parser_t*)llhttp_parser->data;

    /* Reset request. owns_request==true means the previous on_headers_complete
     * never fired (parse error) and we still own the request; destroy it.
     * Otherwise the dispatched request may already have been freed by its
     * handler and `parser->request` is dangling — we must not touch it. */
    if (parser->owns_request && parser->request) {
        http_request_destroy(parser->request);
    }

    parser->request = ecalloc(1, sizeof(http_request_t));
    parser->request->refcount = 1;
    parser->owns_request = true;

    /* Don't allocate headers HashTable yet - do it lazily */
    parser->request->headers = NULL;

    /* Builders are already clean from http_parser_reset_for_reuse or http_parser_create */
    /* DO NOT memset here - it would leak any reused strings! */

    /* Reset state */
    parser->in_header_value = false;
    parser->body_offset = 0;
    parser->total_headers_size = 0;
    parser->parse_error = HTTP_PARSE_OK;

    /* Smuggling-defense state (RFC 9112 §6.3) */
    parser->cl_value_first = 0;
    parser->cl_seen_count = 0;
    parser->header_count = 0;
    parser->te_chunked_seen = false;

    return 0;
}

static int on_url(llhttp_t* llhttp_parser, const char* at, size_t length)
{
    http1_parser_t *parser = (http1_parser_t*)llhttp_parser->data;

    /* Check URI size limit */
    size_t current_len = parser->uri_builder.s ? ZSTR_LEN(parser->uri_builder.s) : 0;
    if (current_len + length > HTTP_MAX_URI_SIZE) {
        parser->parse_error = HTTP_PARSE_ERR_URI_TOO_LONG;
        return -1;  /* URI too long (414 URI Too Long) */
    }

    /* Accumulate URI chunks using smart_str */
    smart_str_appendl(&parser->uri_builder, at, length);

    return 0;
}

static int on_header_field(llhttp_t* llhttp_parser, const char* at, size_t length)
{
    http1_parser_t *parser = (http1_parser_t*)llhttp_parser->data;

    /* Lazy initialization of headers HashTable */
    if (!parser->request->headers) {
        ALLOC_HASHTABLE(parser->request->headers);
        zend_hash_init(parser->request->headers, HTTP_HEADERS_INITIAL_SIZE, NULL, ZVAL_PTR_DTOR, 0);
    }

    /* If we were parsing value, this is a new header - save previous one first */
    if (parser->in_header_value) {
        /* Save previous complete header (also resets builders to NULL) */
        save_current_header(parser);

        /* Reset state flag */
        parser->in_header_value = false;
    }

    /* Check header name size limit */
    size_t current_len = parser->header_name_builder.s ? ZSTR_LEN(parser->header_name_builder.s) : 0;
    if (current_len + length > HTTP_MAX_HEADER_NAME) {
        parser->parse_error = HTTP_PARSE_ERR_HEADER_NAME_TOO_LARGE;
        return -1;  /* Header name too long */
    }

    /* Accumulate header name chunks */
    smart_str_appendl(&parser->header_name_builder, at, length);

    return 0;
}

static int on_header_value(llhttp_t* llhttp_parser, const char* at, size_t length)
{
    http1_parser_t *parser = (http1_parser_t*)llhttp_parser->data;

    /* Must have a header name */
    if (!parser->header_name_builder.s || ZSTR_LEN(parser->header_name_builder.s) == 0) {
        parser->parse_error = HTTP_PARSE_ERR_BAD_HEADER;
        return -1;  /* Error: value without name */
    }

    /* Check header value size limit */
    size_t current_len = parser->header_value_builder.s ? ZSTR_LEN(parser->header_value_builder.s) : 0;
    if (current_len + length > HTTP_MAX_HEADER_VALUE) {
        parser->parse_error = HTTP_PARSE_ERR_HEADER_VALUE_TOO_LARGE;
        return -1;  /* Header value too long */
    }

    /* Check total headers size limit */
    if (parser->total_headers_size + length > HTTP_MAX_HEADERS_TOTAL) {
        parser->parse_error = HTTP_PARSE_ERR_HEADERS_TOTAL_TOO_LARGE;
        return -1;  /* Total headers too large */
    }

    parser->total_headers_size += length;
    parser->in_header_value = true;

    /* Accumulate header value chunks */
    smart_str_appendl(&parser->header_value_builder, at, length);

    return 0;
}

/* Helper: Save accumulated header to HashTable */
static void save_current_header(http1_parser_t *parser)
{
    if (!parser->header_name_builder.s || !parser->header_value_builder.s) {
        return;  /* Nothing to save */
    }

    http_request_t *req = parser->request;

    /* DoS cap on header count (HTTP_MAX_HEADER_COUNT). Prevents an
     * attacker spamming thousands of 1-byte headers to bloat the
     * HashTable. Hit at save time so we surface the error before
     * on_headers_complete dispatches the request. */
    if (UNEXPECTED(parser->header_count >= HTTP_MAX_HEADER_COUNT)) {
        parser->parse_error = HTTP_PARSE_ERR_TOO_MANY_HEADERS;
        return;
    }
    parser->header_count++;

    /* Finalize strings */
    smart_str_0(&parser->header_name_builder);
    smart_str_0(&parser->header_value_builder);

    zend_string *name = parser->header_name_builder.s;
    zend_string *value = parser->header_value_builder.s;

    /* Lowercase header name for case-insensitive lookup */
    zend_str_tolower(ZSTR_VAL(name), ZSTR_LEN(name));

    /* Substitute the builder-allocated name with the process-wide
     * interned copy from http_known_strings when this is a common
     * header (host, content-length, ...). The HashTable then reuses the
     * precomputed hash and skips a per-request alloc on every
     * recognised header — typical REST traffic hits the table on
     * 70-90 %% of headers. The release-at-end below stays correct
     * because zend_string_release is a no-op on interned strings. */
    zend_string *const interned =
        http_known_header_lookup(ZSTR_VAL(name), ZSTR_LEN(name));
    if (interned != NULL) {
        zend_string_release(name);
        parser->header_name_builder.s = NULL;
        name = interned;
    }

    /* Store in HashTable as zval */
    /* RFC 7230 Section 3.2.2: combine duplicate headers with comma */
    zval *existing = zend_hash_find(req->headers, name);
    /* Parse important headers BEFORE the hash insert/combine — once
     * we hit the `existing` branch below `value` gets released, and
     * the old code kept reading from it afterwards (UAF caught by
     * libFuzzer on a duplicate-Connection input; AddressSanitizer
     * SEGV in strncasecmp at src/http1/http_parser.c:314). */
    if (zend_string_equals_literal(name, "content-length")) {
        /* RFC 9110 §8.6 strict numeric form (no sign/whitespace/junk).
         * Bare strtoul silently turned "-1" into UINT_MAX and accepted
         * "100abc"; we now bail out (S-01 audit fix). */
        uint64_t cl;
        if (UNEXPECTED(parse_content_length(ZSTR_VAL(value), ZSTR_LEN(value), &cl) != 0)) {
            parser->parse_error = HTTP_PARSE_ERR_INVALID_CONTENT_LENGTH;
            return;
        }
        /* RFC 9112 §6.3: duplicate Content-Length headers are an error
         * unless every value is identical. Smuggling-class defense
         * because intermediaries can disagree on which CL wins (S-04). */
        if (parser->cl_seen_count == 0) {
            parser->cl_value_first = cl;
        } else if (UNEXPECTED(cl != parser->cl_value_first)) {
            parser->parse_error = HTTP_PARSE_ERR_CONFLICTING_HEADERS;
            return;
        }
        parser->cl_seen_count++;
        req->content_length = (size_t)cl;
    } else if (zend_string_equals_literal(name, "transfer-encoding")) {
        if (strncasecmp(ZSTR_VAL(value), "chunked", 7) == 0) {
            req->chunked = true;
            parser->te_chunked_seen = true;
        }
    } else if (zend_string_equals_literal(name, "connection")) {
        if (strncasecmp(ZSTR_VAL(value), "keep-alive", 10) == 0) {
            req->keep_alive = true;
        } else if (strncasecmp(ZSTR_VAL(value), "close", 5) == 0) {
            req->keep_alive = false;
        }
    }

    if (existing) {
        /* Combine with existing value: "old, new" */
        zend_string *combined = zend_string_concat3(
            Z_STRVAL_P(existing), Z_STRLEN_P(existing),
            ", ", 2,
            ZSTR_VAL(value), ZSTR_LEN(value)
        );
        zend_string_release(value);
        zend_string_release(Z_STR_P(existing));
        ZVAL_STR(existing, combined);
    } else {
        zval zv;
        ZVAL_STR(&zv, value);
        zend_hash_add_new(req->headers, name, &zv);
    }

    /* Release name string (value is now owned by HashTable) */
    zend_string_release(name);

    /* Clear builders (don't free, just reset pointers) */
    parser->header_name_builder.s = NULL;
    parser->header_value_builder.s = NULL;
}

static int on_headers_complete(llhttp_t* llhttp_parser)
{
    http1_parser_t *parser = (http1_parser_t*)llhttp_parser->data;
    http_request_t *req = parser->request;

    /* Admission reject. CoDel/hard-cap
     * may have tripped, or we may be at the in-flight cap. Fail fast
     * with 503 + Retry-After: 1 before touching the body allocator or
     * spawning a handler coroutine. Mirror the same policy on H2 in
     * cb_on_begin_headers via RST_STREAM(REFUSED_STREAM). */
    if (parser->conn != NULL
        && UNEXPECTED(http_server_should_shed_request(parser->conn->server))) {
        http_server_on_request_shed(parser->conn->counters, /*is_h2=*/false);
        parser->parse_error = HTTP_PARSE_ERR_SERVICE_UNAVAILABLE;
        return -1;
    }

    /* Ensure headers HashTable exists (even if no headers were parsed) */
    if (!req->headers) {
        ALLOC_HASHTABLE(req->headers);
        zend_hash_init(req->headers, HTTP_HEADERS_INITIAL_SIZE, NULL, ZVAL_PTR_DTOR, 0);
    }

    /* Save last header if any */
    if (parser->in_header_value) {
        save_current_header(parser);
        parser->in_header_value = false;
    }

    /* Propagate any error save_current_header recorded (invalid CL,
     * conflicting dup-CL, header count overflow). save_current_header
     * is void-typed so we surface it here at the parser boundary. */
    if (UNEXPECTED(parser->parse_error != HTTP_PARSE_OK)) {
        return -1;
    }

    /* RFC 9112 §6.3: reject coexistence of Content-Length and
     * Transfer-Encoding: chunked. Spec text says TE wins, but real-world
     * intermediaries desync on this — refusing the request closes the
     * smuggling vector instead of guessing (S-03). */
    if (UNEXPECTED(parser->cl_seen_count > 0 && parser->te_chunked_seen)) {
        parser->parse_error = HTTP_PARSE_ERR_CONFLICTING_HEADERS;
        return -1;
    }

    /* RFC 9112 §2.5: only HTTP/1.0 and HTTP/1.1 are accepted on this
     * parser. Anything else (HTTP/0.9, 1.2+, 2.0 sneaked through llhttp
     * lenient mode) is rejected with 400 so we never enter the keepalive
     * defaults branch with an unknown semantic. */
    if (UNEXPECTED(llhttp_parser->http_major != 1 ||
                   (llhttp_parser->http_minor != 0 && llhttp_parser->http_minor != 1))) {
        parser->parse_error = HTTP_PARSE_ERR_INVALID_HTTP_VERSION;
        return -1;
    }

    /* Finalize URI */
    if (parser->uri_builder.s) {
        smart_str_0(&parser->uri_builder);
        req->uri = parser->uri_builder.s;
        parser->uri_builder.s = NULL;  /* Transfer ownership */
    }

    /* Get HTTP version */
    req->http_major = llhttp_parser->http_major;
    req->http_minor = llhttp_parser->http_minor;

    /* Get method. Common methods resolve to a process-wide interned
     * zend_string (zero alloc); anything else — extension/WebDAV verbs,
     * custom methods — falls back to the per-request zend_string_init
     * path and still works. */
    const char *method_name = llhttp_method_name(llhttp_parser->method);
    const size_t method_len = strlen(method_name);
    req->method = http_known_method_lookup(method_name, method_len);
    if (req->method == NULL) {
        req->method = zend_string_init(method_name, method_len, 0);
    }

    /* Default keep-alive behavior based on HTTP version */
    /* Check if Connection header was explicitly set */
    zval *connection_header = zend_hash_str_find(req->headers, "connection", sizeof("connection") - 1);
    if (!connection_header) {
        /* Connection header not seen, apply HTTP version defaults */
        if (req->http_major == 1 && req->http_minor >= 1) {
            /* HTTP/1.1: keep-alive by default */
            req->keep_alive = true;
        } else {
            /* HTTP/1.0 and earlier: close by default */
            req->keep_alive = false;
        }
    }

    /* Check for multipart/form-data */
    zval *content_type = zend_hash_str_find(req->headers, "content-type", sizeof("content-type") - 1);
    if (content_type && Z_TYPE_P(content_type) == IS_STRING) {
        const char *ct = Z_STRVAL_P(content_type);
        if (strncasecmp(ct, "multipart/form-data", 19) == 0) {
            char *boundary = extract_boundary(ct);
            if (boundary) {
                req->multipart_proc = mp_processor_create(boundary, NULL);
                if (req->multipart_proc != NULL && parser->conn != NULL) {
                    req->multipart_proc->log_state = parser->conn->log_state;
                }
                req->use_multipart = true;
                efree(boundary);
            }
        }
    }

    /* Prepare body buffer based on Content-Length (only if not multipart) */
    if (!req->use_multipart && req->content_length > 0) {
        /* Check body size limit */
        if (req->content_length > parser->max_body_size) {
            parser->parse_error = HTTP_PARSE_ERR_BODY_TOO_LARGE;
            return -1;  /* 413 Payload Too Large */
        }

        /* Pre-allocate exact size for body. Guard against emalloc OOM
         * (Content-Length can be up to setMaxBodySize; memory_limit may
         * be tighter). On bailout we synthesise a parse error so the
         * connection layer emits 503 and closes cleanly — without this
         * the longjmp escapes the reactor callback and takes the
         * scheduler down with it. Same pattern below for chunked. */
        volatile bool oom = false;
        zend_try {
            req->body = zend_string_alloc(req->content_length, 0);
            parser->body_offset = 0;
        } zend_catch {
            oom = true;
        } zend_end_try();
        if (UNEXPECTED(oom)) {
            parser->parse_error = HTTP_PARSE_ERR_OUT_OF_MEMORY;
            return -1;
        }
    }
    /* For chunked or unknown length, we'll allocate in on_body if needed */

    /* From here on the request belongs to someone else — either dispatch_cb
     * (async path), or the synchronous caller that'll pull it out via
     * http_parser_get_request (see http_parse_request in http_server.c).
     * Flip ownership unconditionally; cleanup paths won't touch `request`
     * after this, which is exactly what we need: the handler may free it
     * before the next on_message_begin fires. */
    parser->owns_request = false;

    if (parser->conn != NULL && parser->conn->view != NULL
        && parser->conn->view->telemetry_enabled) {
        http_request_parse_trace_context(req);
    }

    if (parser->dispatch_cb != NULL) {
        parser->dispatch_cb(parser->conn, req);
    }

    return 0;
}

static int on_body(llhttp_t* llhttp_parser, const char* at, size_t length)
{
    http1_parser_t *parser = (http1_parser_t*)llhttp_parser->data;
    http_request_t *req = parser->request;

    /* If multipart, feed to processor instead */
    if (req->use_multipart && req->multipart_proc) {
        ssize_t result = mp_processor_feed(req->multipart_proc, at, length);
        if (result < 0) {
            parser->parse_error = HTTP_PARSE_ERR_MALFORMED;
            return -1;  /* Multipart parsing error */
        }
        return 0;
    }

    if (req->body) {
        /* Pre-allocated body (Content-Length known) */
        /* Check if we have space using content_length */
        if (parser->body_offset + length > req->content_length) {
            parser->parse_error = HTTP_PARSE_ERR_BODY_TOO_LARGE;
            return -1;  /* Body larger than Content-Length */
        }

        /* Copy directly to pre-allocated buffer */
        memcpy(ZSTR_VAL(req->body) + parser->body_offset, at, length);
        parser->body_offset += length;

        /* Update actual length */
        ZSTR_LEN(req->body) = parser->body_offset;
        ZSTR_VAL(req->body)[parser->body_offset] = '\0';
    } else {
        /* Chunked or unknown length - use smart_str */
        size_t current_len = parser->body_builder.s ? ZSTR_LEN(parser->body_builder.s) : 0;

        /* Check body size limit */
        if (current_len + length > parser->max_body_size) {
            parser->parse_error = HTTP_PARSE_ERR_BODY_TOO_LARGE;
            return -1;  /* Body too large */
        }

        /* Allocate 8KB buffer on first chunk if not allocated yet.
         * Both the first-chunk alloc and the append are OOM-guarded —
         * see on_headers_complete for the rationale. */
        volatile bool oom = false;
        zend_try {
            if (!parser->body_builder.s) {
                smart_str_alloc(&parser->body_builder, HTTP_DEFAULT_BODY_BUFFER, 0);
            }

            /* Size unknown up front (chunked or no Content-Length), so we
             * can't pre-reserve the full body. Scalable-grow switches to
             * doubling once past Zend MM's 2 MiB huge-alloc threshold to
             * cap mremap syscalls at O(log N). See smart_str_scalable.h. */
            http_smart_str_append_scalable(&parser->body_builder, at, length);
        } zend_catch {
            oom = true;
        } zend_end_try();
        if (UNEXPECTED(oom)) {
            parser->parse_error = HTTP_PARSE_ERR_OUT_OF_MEMORY;
            return -1;
        }
    }

    return 0;
}

static int on_message_complete(llhttp_t* llhttp_parser)
{
    http1_parser_t *parser = (http1_parser_t*)llhttp_parser->data;
    http_request_t *req = parser->request;

    /* Finalize multipart processing */
    if (req->use_multipart && req->multipart_proc) {
        finalize_multipart(parser);
    }

    /* Finalize body if using smart_str (chunked/unknown length) */
    if (!req->body && parser->body_builder.s) {
        smart_str_0(&parser->body_builder);
        req->body = parser->body_builder.s;
        parser->body_builder.s = NULL;  /* Transfer ownership */
    }

    req->complete = true;

    /* Notify any coroutine waiting inside $request->awaitBody(). In
     * the default auto_await_body=true path no waiter ever exists
     * (dispatch runs only after message-complete), so this is a cheap
     * no-op. It matters for the streaming dispatch-at-headers-complete
     * mode where the handler coroutine is already running and parked
     * on body_event. */
    if (req->body_event != NULL) {
        zend_async_trigger_event_t *trig =
            (zend_async_trigger_event_t *)req->body_event;
        trig->trigger(trig);
    }

    /* Pause parsing so llhttp doesn't roll straight into the next
     * pipelined request in the same buffer — that would fire
     * on_headers_complete (and dispatch_cb) for request N+1 while
     * request N's handler is still queued, breaking response ordering. */
    return HPE_PAUSED;
}

/* Public API */

http1_parser_t* http_parser_create(size_t max_body_size)
{
    http1_parser_t *parser = ecalloc(1, sizeof(http1_parser_t));

    /* Set configurable limits */
    parser->max_body_size = max_body_size;

    /* smart_str builders already zeroed by ecalloc */

#ifdef HAVE_LLHTTP
    /* Initialize llhttp settings */
    llhttp_settings_init(&parser->settings);
    parser->settings.on_message_begin = on_message_begin;
    parser->settings.on_url = on_url;
    parser->settings.on_header_field = on_header_field;
    parser->settings.on_header_value = on_header_value;
    parser->settings.on_headers_complete = on_headers_complete;
    parser->settings.on_body = on_body;
    parser->settings.on_message_complete = on_message_complete;

    /* Initialize parser */
    llhttp_init(&parser->parser, HTTP_REQUEST, &parser->settings);
    parser->parser.data = parser;
#endif

    return parser;
}

int http_parser_execute(http1_parser_t *parser, const char *data, size_t len, size_t *consumed_out)
{
#ifdef HAVE_LLHTTP
    /* Pipelined request: we paused at the previous on_message_complete.
     * Resume before feeding the next message bytes. */
    if (parser->paused) {
        llhttp_resume(&parser->parser);
        parser->paused = false;
    }

    enum llhttp_errno err = llhttp_execute(&parser->parser, data, len);

    if (consumed_out) {
        /* llhttp_get_error_pos returns the last byte processed before the
         * returned status. For HPE_OK the whole buffer was consumed; for
         * paused states and errors the pointer tells us how far it got. */
        if (err == HPE_OK) {
            *consumed_out = len;
        } else {
            const char *pos = llhttp_get_error_pos(&parser->parser);
            *consumed_out = pos ? (size_t)(pos - data) : 0;
        }
    }

    if (err == HPE_OK) {
        return 0;  /* Success, fully consumed */
    } else if (err == HPE_PAUSED) {
        /* Message complete — we paused to defer pipelined parsing. */
        parser->paused = true;
        return 0;
    } else if (err == HPE_PAUSED_UPGRADE) {
        return 0;  /* Upgrade request, ok */
    } else {
        /* Parse error. If a callback already set parse_error (oversized
         * URI/header/body, bad header) we keep that classification —
         * llhttp surfaces those as HPE_USER. Anything else is an
         * llhttp-detected framing/syntax error → 400 Bad Request. */
        if (parser->parse_error == HTTP_PARSE_OK) {
            parser->parse_error = HTTP_PARSE_ERR_MALFORMED;
        }
        return -1;
    }
#else
    if (consumed_out) {
        *consumed_out = 0;
    }
    return -1;  /* llhttp not available */
#endif
}

http_request_t* http_parser_get_request(const http1_parser_t *parser)
{
    return parser->request;
}


void http_parser_attach(http1_parser_t *parser,
                        http_connection_t *conn,
                        http_dispatch_cb_t dispatch_cb)
{
    parser->conn = conn;
    parser->dispatch_cb = dispatch_cb;
}

void http_parser_reset(http1_parser_t *parser)
{
    if (parser->owns_request && parser->request) {
        http_request_destroy(parser->request);
    }
    parser->request = NULL;
    parser->owns_request = false;

    /* Free smart_str builders */
    smart_str_free(&parser->uri_builder);
    smart_str_free(&parser->body_builder);
    smart_str_free(&parser->header_name_builder);
    smart_str_free(&parser->header_value_builder);

    /* Reset builders */
    memset(&parser->uri_builder, 0, sizeof(smart_str));
    memset(&parser->body_builder, 0, sizeof(smart_str));
    memset(&parser->header_name_builder, 0, sizeof(smart_str));
    memset(&parser->header_value_builder, 0, sizeof(smart_str));

    /* Reset state */
    parser->in_header_value = false;
    parser->paused = false;
    parser->body_offset = 0;
    parser->total_headers_size = 0;
    parser->parse_error = HTTP_PARSE_OK;

#ifdef HAVE_LLHTTP
    llhttp_init(&parser->parser, HTTP_REQUEST, &parser->settings);
    parser->parser.data = parser;
#endif
}

void http_parser_destroy(http1_parser_t *parser)
{
    if (parser->owns_request && parser->request) {
        http_request_destroy(parser->request);
    }
    parser->request = NULL;
    parser->owns_request = false;

    /* Free smart_str builders */
    smart_str_free(&parser->uri_builder);
    smart_str_free(&parser->body_builder);
    smart_str_free(&parser->header_name_builder);
    smart_str_free(&parser->header_value_builder);

    efree(parser);
}

void http_request_addref(http_request_t *req)
{
    if (req != NULL) {
        req->refcount++;
    }
}

void http_request_destroy(http_request_t *req)
{
    if (!req) {
        return;
    }
    /* Refcount-based release. Each holder calls destroy when done; the
     * last call (refcount → 0) actually frees. Allocators init refcount
     * to 1 so a single-owner caller's destroy still frees immediately —
     * preserves the pre-refcount behavior at every existing call site. */
    if (--req->refcount > 0) {
        return;
    }

    if (req->method) {
        zend_string_release(req->method);
    }

    if (req->uri) {
        zend_string_release(req->uri);
    }

    if (req->headers) {
        zend_hash_destroy(req->headers);
        FREE_HASHTABLE(req->headers);
    }

    if (req->body) {
        zend_string_release(req->body);
    }

    /* Dispose body-progress event if awaitBody() created one */
    if (req->body_event) {
        req->body_event->dispose(req->body_event);
        req->body_event = NULL;
    }

    /* Cleanup multipart state (owned by the request) */
    if (req->multipart_proc) {
        mp_processor_cleanup_temp_files(req->multipart_proc);
        mp_processor_destroy(req->multipart_proc);
    }
    if (req->post_data) {
        zend_hash_destroy(req->post_data);
        FREE_HASHTABLE(req->post_data);
    }
    if (req->files) {
        zend_hash_destroy(req->files);
        FREE_HASHTABLE(req->files);
    }

    if (req->traceparent_raw) {
        zend_string_release(req->traceparent_raw);
    }
    if (req->tracestate_raw) {
        zend_string_release(req->tracestate_raw);
    }

    efree(req);
}

/* Helper: Reset smart_str with buffer reuse if possible */
static inline void smart_str_reset_or_free(smart_str *str)
{
    if (can_reuse_string_buffer(str->s)) {
        /* Only we own it - reuse buffer! */
        ZSTR_LEN(str->s) = 0;
        ZSTR_VAL(str->s)[0] = '\0';
    } else if (str->s) {
        /* Someone else owns it - release and reset */
        smart_str_free(str);
        memset(str, 0, sizeof(smart_str));
    }
}

/* {{{ http_parse_error_to_status / http_parse_error_reason
 *
 * Static mapping from parser-level error codes to RFC-compliant 4xx
 * status + single-line reason phrase. Reason text is also used as the
 * response body (matches envoy/nginx minimalism). All literals live
 * in .rodata.
 */
int http_parse_error_to_status(http_parse_error_t err)
{
    switch (err) {
        case HTTP_PARSE_ERR_URI_TOO_LONG:            return 414;
        case HTTP_PARSE_ERR_HEADER_NAME_TOO_LARGE:   return 431;
        case HTTP_PARSE_ERR_HEADER_VALUE_TOO_LARGE:  return 431;
        case HTTP_PARSE_ERR_HEADERS_TOTAL_TOO_LARGE: return 431;
        case HTTP_PARSE_ERR_TOO_MANY_HEADERS:        return 431;
        case HTTP_PARSE_ERR_BODY_TOO_LARGE:          return 413;
        case HTTP_PARSE_ERR_BAD_HEADER:              return 400;
        case HTTP_PARSE_ERR_MALFORMED:               return 400;
        case HTTP_PARSE_ERR_INVALID_CONTENT_LENGTH:  return 400;
        case HTTP_PARSE_ERR_CONFLICTING_HEADERS:     return 400;
        case HTTP_PARSE_ERR_INVALID_HTTP_VERSION:    return 400;
        case HTTP_PARSE_ERR_OUT_OF_MEMORY:           return 503;
        case HTTP_PARSE_ERR_SERVICE_UNAVAILABLE:     return 503;
        case HTTP_PARSE_OK:
        default:                                     return 400;
    }
}

const char *http_parse_error_reason(http_parse_error_t err)
{
    switch (err) {
        case HTTP_PARSE_ERR_URI_TOO_LONG:            return "URI Too Long";
        case HTTP_PARSE_ERR_HEADER_NAME_TOO_LARGE:   return "Request Header Fields Too Large";
        case HTTP_PARSE_ERR_HEADER_VALUE_TOO_LARGE:  return "Request Header Fields Too Large";
        case HTTP_PARSE_ERR_HEADERS_TOTAL_TOO_LARGE: return "Request Header Fields Too Large";
        case HTTP_PARSE_ERR_TOO_MANY_HEADERS:        return "Request Header Fields Too Large";
        case HTTP_PARSE_ERR_BODY_TOO_LARGE:          return "Content Too Large";
        case HTTP_PARSE_ERR_BAD_HEADER:              return "Bad Request";
        case HTTP_PARSE_ERR_MALFORMED:               return "Bad Request";
        case HTTP_PARSE_ERR_INVALID_CONTENT_LENGTH:  return "Bad Request";
        case HTTP_PARSE_ERR_CONFLICTING_HEADERS:     return "Bad Request";
        case HTTP_PARSE_ERR_INVALID_HTTP_VERSION:    return "Bad Request";
        case HTTP_PARSE_ERR_OUT_OF_MEMORY:           return "Service Unavailable";
        case HTTP_PARSE_ERR_SERVICE_UNAVAILABLE:     return "Service Unavailable";
        case HTTP_PARSE_OK:
        default:                                     return "Bad Request";
    }
}
/* }}} */

/* Reset parser for reuse with buffer recycling based on refcount */
void http_parser_reset_for_reuse(http1_parser_t *parser)
{
    if (parser->owns_request && parser->request) {
        http_request_destroy(parser->request);
    }
    parser->request = NULL;
    parser->owns_request = false;

    /* Clear dispatch wiring — new user must re-attach */
    parser->conn = NULL;
    parser->dispatch_cb = NULL;

    /* Reset all builders (with buffer reuse if possible) */
    smart_str_reset_or_free(&parser->uri_builder);
    smart_str_reset_or_free(&parser->body_builder);
    smart_str_reset_or_free(&parser->header_name_builder);
    smart_str_reset_or_free(&parser->header_value_builder);

    /* Reset state */
    parser->in_header_value = false;
    parser->body_offset = 0;
    parser->total_headers_size = 0;
    parser->parse_error = HTTP_PARSE_OK;

#ifdef HAVE_LLHTTP
    /* Reinitialize llhttp parser */
    llhttp_init(&parser->parser, HTTP_REQUEST, &parser->settings);
    parser->parser.data = parser;
#endif
}
