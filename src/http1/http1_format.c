/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

/* HTTP/1.x response wire-format encoder. H/2 + H/3 use only the
 * status-reason lookup here; binary framing lives in src/http2/,
 * src/http3/. */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include "php.h"
#include "zend_smart_str.h"
#include "php_http_server.h"
#include "http_response_internal.h"
#include "http1/http1_stream.h"
#include "http_date.h"
#include <time.h>

#ifdef HAVE_HTTP_COMPRESSION
# include "compression/http_compression_response.h"
#endif

/* HTTP status reason phrases. Internal cross-TU: http_response.c
 * needs it for getReasonPhrase() when no custom phrase was set. */
const char *http_status_reason(int code)
{
    switch (code) {
        case 100: return "Continue";
        case 101: return "Switching Protocols";
        case 200: return "OK";
        case 201: return "Created";
        case 202: return "Accepted";
        case 204: return "No Content";
        case 205: return "Reset Content";
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
    [205] = MK_LINE("HTTP/1.1 205 Reset Content\r\n"),
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
    [416] = MK_LINE("HTTP/1.1 416 Range Not Satisfiable\r\n"),
    [429] = MK_LINE("HTTP/1.1 429 Too Many Requests\r\n"),
    [500] = MK_LINE("HTTP/1.1 500 Internal Server Error\r\n"),
    [502] = MK_LINE("HTTP/1.1 502 Bad Gateway\r\n"),
    [503] = MK_LINE("HTTP/1.1 503 Service Unavailable\r\n"),
    [504] = MK_LINE("HTTP/1.1 504 Gateway Timeout\r\n"),
};
#undef MK_LINE
#define HTTP11_STATUS_LINES_CNT \
    (sizeof(http11_status_lines) / sizeof(http11_status_lines[0]))

const char *http_response_status_line_http11(const int code, size_t *out_len)
{
    if (code <= 0 || (size_t) code >= HTTP11_STATUS_LINES_CNT) {
        return NULL;
    }

    const http_status_line_t *e = &http11_status_lines[code];

    if (e->line == NULL) {
        return NULL;
    }

    if (out_len != NULL) {
        *out_len = e->len;
    }

    return e->line;
}

bool h1_response_peer_speaks_http11(zend_object *response_obj)
{
    const http_response_object *response = http_response_from_obj(response_obj);

    /* Asked as the rule is written — "the request indicated 1.1 or later"
     * (RFC 9112 §6.1) — rather than as "not 1.0". The parser admits only 1.0
     * and 1.1 today, so the two agree; they stop agreeing the moment anything
     * else gets through, and this answer decides how a body is framed. A
     * response built outside a connection carries no version and counts as
     * 1.1, which is what every modern peer reads. */
    return response->protocol_version == NULL
        || (ZSTR_LEN(response->protocol_version) == 3
            && memcmp(ZSTR_VAL(response->protocol_version), "1.1", 3) == 0);
}

/* RFC 9110 §8.6: a 1xx or a 204 carries no Content-Length, whoever set it. A
 * 304 is the exception in the other direction — §8.6 permits the field there,
 * and wants it to
 * describe the representation the 200 would have carried. */
static inline bool response_status_forbids_content_length(const int status)
{
    return status < 200 || status == 204;
}

h1_framing_t h1_response_framing(zend_object *response_obj)
{
    const http_response_object *response = http_response_from_obj(response_obj);

    if (response->is_head || !response_status_carries_body(response->status_code)) {
        return H1_FRAMING_NONE;
    }

    if (response->declared_length >= 0) {
        return H1_FRAMING_LENGTH;
    }

    return h1_response_peer_speaks_http11(response_obj)
        ? H1_FRAMING_CHUNKED : H1_FRAMING_CLOSE;
}

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

/* Single-grow header line writer. "name: value\r\n" written with one
 * smart_str_extend (one grow-check + tail pointer) instead of 4×
 * smart_str_append* which pays the size-check+memcpy bookkeeping per
 * call. The two memcpy's (name, value) are inherent — name and value
 * live in separate zend_strings. */
static inline void append_header_line(smart_str *out,
                                      const char *name, size_t name_len,
                                      const char *value, size_t value_len)
{
    const size_t need = name_len + 2 /* ": " */ + value_len + 2 /* "\r\n" */;
    char *p = smart_str_extend(out, need);
    memcpy(p, name, name_len);            p += name_len;
    *p++ = ':'; *p++ = ' ';
    memcpy(p, value, value_len);          p += value_len;
    *p++ = '\r'; *p   = '\n';
}

/* Case-insensitive — handlers may set either casing. */
static bool header_is_content_length(const zend_string *name)
{
    return ZSTR_LEN(name) == 14
        && zend_binary_strcasecmp(ZSTR_VAL(name), 14, "content-length", 14) == 0;
}

static bool header_is_transfer_encoding(const zend_string *name)
{
    return ZSTR_LEN(name) == 17
        && zend_binary_strcasecmp(ZSTR_VAL(name), 17, "transfer-encoding", 17) == 0;
}

static inline void emit_content_length(smart_str *out, const size_t body_len)
{
    smart_str_appendl(out, "Content-Length: ", sizeof("Content-Length: ") - 1);
    smart_str_append_unsigned(out, body_len);
    smart_str_appendl(out, "\r\n", 2);
}

/* Iterate the headers table emitting "name: value\r\n" for each entry.
 * The two framing headers are dropped separately, because a declared-length
 * stream keeps its Content-Length while still refusing a handler's
 * Transfer-Encoding: one body cannot be framed twice (RFC 9112 §6.1), and a
 * response carrying both is what an intermediary resolves one way and its peer
 * the other. Flat IS_STRING fast path (single-value, common case) + IS_ARRAY
 * multi-value fallback. Does NOT emit the trailing CRLF that ends the header
 * block — caller appends that. */
static void emit_headers_only(smart_str *out, HashTable *headers,
                              const bool drop_content_length,
                              const bool drop_transfer_encoding)
{
    zend_string *name;
    zval *values;
    ZEND_HASH_FOREACH_STR_KEY_VAL(headers, name, values) {
        if (UNEXPECTED(name == NULL)) continue;

        if (drop_content_length && header_is_content_length(name)) continue;

        if (drop_transfer_encoding && header_is_transfer_encoding(name)) continue;

        if (EXPECTED(Z_TYPE_P(values) == IS_STRING)) {
            const zend_string *v = Z_STR_P(values);
            append_header_line(out, ZSTR_VAL(name), ZSTR_LEN(name),
                               ZSTR_VAL(v), ZSTR_LEN(v));
        } else if (Z_TYPE_P(values) == IS_ARRAY) {
            zval *val;
            ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(values), val) {
                if (Z_TYPE_P(val) != IS_STRING) continue;
                const zend_string *v = Z_STR_P(val);
                append_header_line(out, ZSTR_VAL(name), ZSTR_LEN(name),
                                   ZSTR_VAL(v), ZSTR_LEN(v));
            } ZEND_HASH_FOREACH_END();
        }
    } ZEND_HASH_FOREACH_END();
}

/* RFC 9110 §6.6.1: an origin server MUST send a Date header. Emit one
 * (IMF-fixdate, generated now) unless the handler already set it. */
static void emit_date_header(smart_str *out, HashTable *headers)
{
    if (headers != NULL
        && zend_hash_str_exists(headers, "date", sizeof("date") - 1)) {
        return;
    }

    char datebuf[HTTP_DATE_BUF_LEN];
    http_date_format_imf(time(NULL), datebuf);
    smart_str_appendl(out, "Date: ", sizeof("Date: ") - 1);
    smart_str_appendl(out, datebuf, HTTP_DATE_LEN);
    smart_str_appendl(out, "\r\n", 2);
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

    emit_date_header(result, response->headers);

    /* Content-Length on a buffered body is the server's to state: it is holding
     * the bytes it is about to send, so a handler value that disagrees would
     * put the peer's read cursor at the wrong byte and desync a reused
     * connection. Use zend_hash_str_exists to skip the zend_string
     * alloc/release round-trip on the literal name lookup. */
    const bool handler_declared =
        zend_hash_str_exists(response->headers, "content-length",
                             sizeof("content-length") - 1);

    /* The handler's number is kept on a HEAD, where the message carries no body
     * to measure it against and the value describes the one a GET would have
     * returned. A status that ends at the blank line keeps it only where the
     * number still means something — see the branch below.
     *
     * Three branches rather than one computed flag: the common response
     * declares no length, and passing the literal lets the emit loop drop the
     * name check for it. */
    const bool carries_body = response_status_carries_body(response->status_code);
    const bool body_follows = !response->is_head && carries_body;

    if (UNEXPECTED(!carries_body)) {
        /* RFC 9112 §6.3 rule 1: the message ends at the blank line, so the
         * server has no count to state. A 304 keeps the handler's, which
         * describes the representation a 200 would have carried (RFC 9110
         * §8.6); a 1xx and a 204 lose it, which the same section requires.
         *
         * A 205 is the exception rule 1 does not cover: the peer will look for
         * framing, so the emptiness is stated rather than implied. */
        if (response_status_needs_zero_length(response->status_code)) {
            emit_content_length(result, 0);
            emit_headers_only(result, response->headers, true, true);
        } else {
            emit_headers_only(result, response->headers,
                              response_status_forbids_content_length(response->status_code), true);
        }
    } else if (handler_declared && body_follows) {
        emit_content_length(result, body_len);
        emit_headers_only(result, response->headers, true, true);
    } else if (response->head_streamed && !handler_declared) {
        /* A HEAD whose handler produced its body by streaming. The bytes were
         * dropped, so the buffer holds nothing to measure, and stating its
         * length says the GET body is empty — the opposite of what RFC 9110
         * §9.3.2 wants the field to mean. §9.3.2 lets a server omit a field
         * whose value is only known while generating the content; a handler
         * that knows the count declares it before its first streaming call. */
        emit_headers_only(result, response->headers, false, true);
    } else {
        if (!handler_declared) {
            emit_content_length(result, body_len);
        }

        emit_headers_only(result, response->headers, false, true);
    }

    /* End of headers */
    smart_str_appendl(result, "\r\n", 2);
}

/* Body length for the threshold branch in the dispose hot path. Reads
 * the smart_str's terminator-safe length without exposing the response
 * struct. Returns 0 when no body has been buffered. */
size_t http_response_get_body_len(zend_object *obj)
{
    http_response_object *response = http_response_from_obj(obj);
    if (response->body_view != NULL) {
        return ZSTR_LEN(response->body_view);
    }

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
    /* Pre-size for a typical 10-15 header block (~500 bytes) plus
     * comfortable headroom — saves the realloc rounds on the
     * smart_str grow path. */
    smart_str_alloc(&result, 1024, 0);

#ifdef HAVE_HTTP_COMPRESSION
    http_compression_apply_buffered(obj);
#endif

    zend_string *const view = response->body_view;
    if (view == NULL) {
        smart_str_0(&response->body);
    }

    const size_t body_len = view != NULL
        ? ZSTR_LEN(view)
        : (response->body.s ? ZSTR_LEN(response->body.s) : 0);

    emit_headers_block(&result, response, body_len);
    smart_str_0(&result);

    *headers_out = result.s ? result.s : zend_empty_string;

    /* No body part on a message that ends at the blank line — see the same
     * rule in http_response_format. */
    if (UNEXPECTED(!response_status_carries_body(response->status_code))) {
        *body_out = NULL;
    } else if (view != NULL) {
        zend_string_addref(view);
        *body_out = view;
    } else if (response->body.s != NULL && body_len > 0) {
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
    smart_str_alloc(&result, 1024, 0);

#ifdef HAVE_HTTP_COMPRESSION
    http_compression_apply_buffered(obj);
#endif

    zend_string *const view = response->body_view;
    if (view == NULL) {
        smart_str_0(&response->body);
    }

    const size_t body_len = view != NULL
        ? ZSTR_LEN(view)
        : (response->body.s ? ZSTR_LEN(response->body.s) : 0);

    emit_headers_block(&result, response, body_len);

    /* The message ends at the blank line on a 1xx, a 204 and a 304 (RFC 9112
     * §6.3 rule 1). Bytes appended past it are read as the next message's
     * status line, which desynchronises a connection the peer may reuse. */
    if (EXPECTED(response_status_carries_body(response->status_code))) {
        if (view != NULL) {
            smart_str_append(&result, view);
        } else if (response->body.s && body_len > 0) {
            smart_str_append(&result, response->body.s);
        }
    }

    smart_str_0(&result);

    return result.s ? result.s : zend_empty_string;
}

/* Serialize status line + headers for an HTTP/1 streaming response. Headers
 * end with the empty line; the caller writes the body after them.
 *
 * The block announces whichever framing h1_response_framing picked, and only
 * chunked coding has a header of its own to announce. A Content-Length
 * survives in two of the four: the declared length the server is auditing, and
 * a handler value on a message with no body, where the number describes
 * something else the peer still wants — the representation a 304 stands for,
 * the body a GET would have returned for a HEAD. A handler Transfer-Encoding
 * is dropped in all four: the framing is the server's to state, and the pair
 * with Content-Length is the shape RFC 9112 §6.3 names as smuggling.
 *
 * Used by h1_stream_ops at first write(). Separate from http_response_format
 * because the latter builds status + Content-Length + headers + body
 * as a single atomic payload, which is exactly what streaming avoids. */
zend_string *http_response_format_streaming_headers(zend_object *obj)
{
    http_response_object *response = http_response_from_obj(obj);
    const h1_framing_t framing = h1_response_framing(obj);
    const bool drop_content_length = framing == H1_FRAMING_NONE
        ? response_status_forbids_content_length(response->status_code)
        : framing != H1_FRAMING_LENGTH;
    /* A 205 states a zero length wherever it is framed. No streaming call can
     * reach this today — the shared guard refuses a status that carries no
     * body before any of them commits — but the rule lives in
     * http_response_internal.h and both formatters have to honour it, or the
     * next path to reach here inherits the gap the 205 leg was built to close. */
    const bool state_zero_length =
        response_status_needs_zero_length(response->status_code);
    smart_str result = {0};
    smart_str_alloc(&result, 1024, 0);

    emit_status_line(&result, response);
    emit_date_header(&result, response->headers);

    if (framing == H1_FRAMING_CHUNKED) {
        smart_str_appendl(&result, "Transfer-Encoding: chunked\r\n",
                          sizeof("Transfer-Encoding: chunked\r\n") - 1);
    }

    if (state_zero_length) {
        emit_content_length(&result, 0);
    }

    emit_headers_only(&result, response->headers,
                      drop_content_length || state_zero_length, true);

    smart_str_appendl(&result, "\r\n", 2);
    smart_str_0(&result);
    return result.s ? result.s : zend_empty_string;
}

/* Static-handler head builder (issue #13 sendfile path). Status line +
 * verbatim headers (NO auto-Content-Length — caller is responsible:
 * the static handler sets it from file size, omits it on 304) +
 * terminator + optional inline body (small 4xx/416 text). Does NOT
 * run the compression hook (file body rides separately via sendfile;
 * the inline body buffer is tiny error text where compression would
 * be wasteful).
 *
 * Returned zend_string is owned by the caller. */
zend_string *http_response_format_static_head(zend_object *obj,
                                              bool include_inline_body)
{
    http_response_object *response = http_response_from_obj(obj);
    smart_str result = {0};
    smart_str_alloc(&result, 1024, 0);

    emit_status_line(&result, response);
    emit_headers_only(&result, response->headers, false, false);
    smart_str_appendl(&result, "\r\n", 2);

    if (include_inline_body) {
        if (response->body_view != NULL) {
            smart_str_append(&result, response->body_view);
        } else {
            smart_str_0(&response->body);

            if (response->body.s != NULL && ZSTR_LEN(response->body.s) > 0) {
                smart_str_append(&result, response->body.s);
            }
        }
    }

    smart_str_0(&result);
    return result.s ? result.s : zend_empty_string;
}
