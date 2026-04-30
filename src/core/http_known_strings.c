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

#include "http_known_strings.h"

#include <string.h>

/* The 9 RFC 9110 methods. Extension methods (e.g. WebDAV PROPFIND, or
 * anything custom) fall through to the zend_string_init path and keep
 * working — this table is a fast-path cache, not a whitelist. */
static zend_string *known_methods[9];

static const struct {
    const char *name;
    size_t      len;
} known_method_table[9] = {
    {"GET",     3},
    {"POST",    4},
    {"PUT",     3},
    {"DELETE",  6},
    {"HEAD",    4},
    {"PATCH",   5},
    {"OPTIONS", 7},
    {"TRACE",   5},
    {"CONNECT", 7},
};

/* Lowercase HTTP header names that show up on virtually every request.
 * Ordered by observed frequency on REST/RPC traffic — first hit wins
 * the linear scan. Extension headers fall through and take the
 * zend_string_init path, so this is a fast-path cache, not a whitelist.
 *
 * Names are lowercase to match how both pipelines deliver them:
 *   - HTTP/1: save_current_header() lowercases via zend_str_tolower
 *     before calling the lookup.
 *   - HTTP/2: nghttp2 enforces RFC 9113 §8.2.1 (lowercase names on
 *     wire) and rejects mixed case before cb_on_header fires.
 *
 * Keep the list short — every miss costs a full scan. The 23 names
 * below cover the headers that actually repeat per request in practice;
 * adding rarer ones makes the miss path slower without speeding up the
 * common case. */
#define HTTP_KNOWN_HEADER_COUNT 23
static zend_string *known_headers[HTTP_KNOWN_HEADER_COUNT];

static const struct {
    const char *name;
    size_t      len;
} known_header_table[HTTP_KNOWN_HEADER_COUNT] = {
    /* Always-present on real traffic */
    {"host",                4},
    {"user-agent",         10},
    {"accept",              6},
    {"accept-encoding",    15},
    {"accept-language",    15},
    {"connection",         10},
    {"content-length",     14},
    {"content-type",       12},
    /* Conditional GET / caching */
    {"cache-control",      13},
    {"if-modified-since",  17},
    {"if-none-match",      13},
    {"etag",                4},
    /* Auth + cookies */
    {"authorization",      13},
    {"cookie",              6},
    /* CORS / origin tracking */
    {"origin",              6},
    {"referer",             7},
    /* Proxies / forwarding */
    {"x-forwarded-for",    15},
    {"x-forwarded-proto",  17},
    {"x-real-ip",           9},
    /* Body framing */
    {"transfer-encoding",  17},
    /* H2 mappings + uploads */
    {"scheme",              6},   /* H2 :scheme stored under this name */
    {"range",               5},
    {"upgrade",             7},
};

void http_known_strings_minit(void)
{
    for (size_t i = 0; i < sizeof(known_method_table) / sizeof(known_method_table[0]); i++) {
        known_methods[i] = zend_string_init_interned(
            known_method_table[i].name,
            known_method_table[i].len,
            /* permanent */ 1
        );
    }
    for (size_t i = 0; i < HTTP_KNOWN_HEADER_COUNT; i++) {
        known_headers[i] = zend_string_init_interned(
            known_header_table[i].name,
            known_header_table[i].len,
            /* permanent */ 1
        );
    }
}

zend_string *http_known_method_lookup(const char *name, size_t len)
{
    /* Linear scan over 9 entries: predictable branch, fits in one cache
     * line, beats a hash lookup at this size. Ordered by observed
     * frequency in REST traffic (GET/POST first). */
    for (size_t i = 0; i < sizeof(known_method_table) / sizeof(known_method_table[0]); i++) {
        if (known_method_table[i].len == len
            && memcmp(known_method_table[i].name, name, len) == 0) {
            return known_methods[i];
        }
    }
    return NULL;
}

zend_string *http_known_header_lookup(const char *name, size_t len)
{
    /* Same shape as method lookup. The table is small enough (23 entries)
     * that a length pre-filter + memcmp beats hashed dispatch — and the
     * caller has already paid for the lowercase, so we skip strncasecmp. */
    for (size_t i = 0; i < HTTP_KNOWN_HEADER_COUNT; i++) {
        if (known_header_table[i].len == len
            && memcmp(known_header_table[i].name, name, len) == 0) {
            return known_headers[i];
        }
    }
    return NULL;
}
