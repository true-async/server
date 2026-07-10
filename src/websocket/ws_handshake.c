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
#include "websocket/ws_handshake.h"
#include "http1/http_parser.h"   /* http_request_t layout */

#include "ext/standard/sha1.h"
#include "ext/standard/base64.h"

#include <string.h>
#ifndef PHP_WIN32
# include <strings.h>
#endif

/* {{{ header_lookup
 *
 * Case-insensitive single-value header read. http_request_t stores
 * headers in a HashTable keyed by lowercased name (see http_parser.c
 * save_current_header), so a direct lowercase probe suffices. Returns
 * the underlying string and length, or NULL on miss.
 */
static const char *header_lookup(const http_request_t *req,
                                 const char *name, size_t name_len,
                                 size_t *out_len)
{
    if (req->headers == NULL) {
        return NULL;
    }
    zval *val = zend_hash_str_find(req->headers, name, name_len);
    if (val == NULL || Z_TYPE_P(val) != IS_STRING) {
        return NULL;
    }
    *out_len = Z_STRLEN_P(val);
    return Z_STRVAL_P(val);
}
/* }}} */

/* {{{ contains_token_ci
 *
 * Does a comma-separated header value contain `token` (case-insensitive,
 * trimmed)? Used for Connection: keep-alive, Upgrade — clients send
 * combinations like "Upgrade, keep-alive" or "keep-alive, Upgrade".
 */
static bool contains_token_ci(const char *value, size_t value_len,
                              const char *token, size_t token_len)
{
    size_t i = 0;
    while (i < value_len) {
        /* Skip leading whitespace + commas. */
        while (i < value_len && (value[i] == ' ' || value[i] == '\t' || value[i] == ',')) {
            i++;
        }
        size_t start = i;
        while (i < value_len && value[i] != ',') {
            i++;
        }
        size_t end = i;
        /* Trim trailing whitespace. */
        while (end > start && (value[end - 1] == ' ' || value[end - 1] == '\t')) {
            end--;
        }
        if (end - start == token_len &&
            strncasecmp(value + start, token, token_len) == 0) {
            return true;
        }
    }
    return false;
}
/* }}} */

ws_handshake_result_t ws_handshake_validate(const http_request_t *req)
{
    /* RFC 6455 §4.1 step 1: request method MUST be GET.
     * Cheapest probe first — most non-WS requests bail here. */
    size_t upgrade_len = 0;
    const char *upgrade = header_lookup(req, "upgrade", sizeof("upgrade") - 1,
                                        &upgrade_len);
    if (upgrade == NULL) {
        /* No Upgrade header — definitely not a WS request. The vast
         * majority of HTTP traffic exits here after one hash probe. */
        return WS_HANDSHAKE_NOT_AN_UPGRADE;
    }

    /* Upgrade header present — from here failures translate to HTTP
     * status codes per RFC 6455. Method check first because GET is
     * the only allowed verb. */
    if (req->method == NULL ||
        ZSTR_LEN(req->method) != 3 ||
        memcmp(ZSTR_VAL(req->method), "GET", 3) != 0) {
        return WS_HANDSHAKE_FORBIDDEN_METHOD;
    }

    /* RFC 6455 §4.1 step 3: Upgrade header MUST contain "websocket"
     * (case-insensitive). The whole field is "websocket" in practice
     * — multi-token Upgrade is theoretical. */
    if (!contains_token_ci(upgrade, upgrade_len, "websocket", 9)) {
        return WS_HANDSHAKE_BAD_REQUEST;
    }

    /* RFC 6455 §4.1 step 4: Connection header MUST contain "Upgrade". */
    size_t connection_len = 0;
    const char *connection = header_lookup(req, "connection",
                                           sizeof("connection") - 1,
                                           &connection_len);
    if (connection == NULL ||
        !contains_token_ci(connection, connection_len, "upgrade", 7)) {
        return WS_HANDSHAKE_BAD_REQUEST;
    }

    /* RFC 6455 §4.1 step 7: Sec-WebSocket-Version MUST equal 13.
     * Per RFC the failure response is 426 Upgrade Required with a
     * Sec-WebSocket-Version: 13 hint header — distinct from a generic
     * 400 so clients with newer drafts can downgrade. */
    size_t version_len = 0;
    const char *version = header_lookup(req, "sec-websocket-version",
                                        sizeof("sec-websocket-version") - 1,
                                        &version_len);
    if (version == NULL || version_len != 2 ||
        version[0] != '1' || version[1] != '3') {
        return WS_HANDSHAKE_UPGRADE_REQUIRED;
    }

    /* RFC 6455 §4.1 step 5/6: Sec-WebSocket-Key MUST be present,
     * 16-byte value base64-encoded — exactly 24 wire characters.
     * We do not decode here (the SHA-1 input concatenates the raw
     * wire form per §4.2.2), only length-validate. */
    size_t key_len = 0;
    const char *key = header_lookup(req, "sec-websocket-key",
                                    sizeof("sec-websocket-key") - 1,
                                    &key_len);
    if (key == NULL || key_len != WS_CLIENT_KEY_LEN) {
        return WS_HANDSHAKE_BAD_REQUEST;
    }

    /* RFC 9112 §2.5: HTTP/1.1 required for Upgrade semantics. The H1
     * parser already rejects anything other than 1.0/1.1; here we only
     * need to exclude 1.0 because Connection/Upgrade is undefined on
     * pre-1.1. */
    if (req->http_major != 1 || req->http_minor < 1) {
        return WS_HANDSHAKE_BAD_REQUEST;
    }

    return WS_HANDSHAKE_OK;
}

int ws_handshake_compute_accept(const char *client_key, size_t client_key_len,
                                char *out)
{
    if (client_key_len != WS_CLIENT_KEY_LEN) {
        return -1;
    }

    PHP_SHA1_CTX ctx;
    unsigned char digest[20];

    PHP_SHA1Init(&ctx);
    PHP_SHA1Update(&ctx, (const unsigned char *)client_key, client_key_len);
    PHP_SHA1Update(&ctx, (const unsigned char *)WS_MAGIC_GUID,
                   sizeof(WS_MAGIC_GUID) - 1);
    PHP_SHA1Final(digest, &ctx);

    /* base64(20 bytes) is always exactly 28 chars including padding —
     * matches WS_ACCEPT_LEN. Copy without trailing NUL; caller composes
     * the header line. */
    zend_string *encoded = php_base64_encode(digest, sizeof(digest));
    if (encoded == NULL) {
        return -1;
    }
    ZEND_ASSERT(ZSTR_LEN(encoded) == WS_ACCEPT_LEN);
    memcpy(out, ZSTR_VAL(encoded), WS_ACCEPT_LEN);
    zend_string_release(encoded);

    return 0;
}

zend_string *ws_handshake_build_101_response(const char *accept,
                                             const char *subprotocol,
                                             const int deflate_bits)
{
    /* Status line + 4 mandatory headers come to ~140 bytes; subprotocol
     * adds ~30 + token. One smart_str grow is enough — preallocate. */
    smart_str buf = {0};
    smart_str_alloc(&buf, 256, 0);

    smart_str_appends(&buf, "HTTP/1.1 101 Switching Protocols\r\n");
    smart_str_appends(&buf, "Upgrade: websocket\r\n");
    smart_str_appends(&buf, "Connection: Upgrade\r\n");
    smart_str_appends(&buf, "Sec-WebSocket-Accept: ");
    smart_str_appendl(&buf, accept, WS_ACCEPT_LEN);
    smart_str_appends(&buf, "\r\n");

    if (subprotocol != NULL && *subprotocol != '\0') {
        /* Caller is responsible for picking a token from the client's
         * Sec-WebSocket-Protocol offer list. We do not re-validate the
         * token grammar (RFC 7230 token chars) here — invalid tokens
         * would have been rejected at the WebSocketUpgrade::setSubprotocol
         * boundary in the PHP layer. */
        smart_str_appends(&buf, "Sec-WebSocket-Protocol: ");
        smart_str_appends(&buf, subprotocol);
        smart_str_appends(&buf, "\r\n");
    }

    if (deflate_bits > 0) {
        /* no_context_takeover both ways → we reset the deflate/inflate
         * streams per message (RFC 7692 §7.1.1). Below-default window
         * bits are echoed so the peer knows we honoured its cap. */
        smart_str_appends(&buf, "Sec-WebSocket-Extensions: permessage-deflate; "
            "server_no_context_takeover; client_no_context_takeover");

        if (deflate_bits < 15) {
            char bits[40];
            const int n = snprintf(bits, sizeof(bits),
                                   "; server_max_window_bits=%d", deflate_bits);
            smart_str_appendl(&buf, bits, (size_t)n);
        }

        smart_str_appends(&buf, "\r\n");
    }

    smart_str_appends(&buf, "\r\n");
    smart_str_0(&buf);

    return buf.s;
}

int ws_pmce_negotiate(const http_request_t *req)
{
    size_t ext_len = 0;
    const char *ext = header_lookup(req, "sec-websocket-extensions",
                                    sizeof("sec-websocket-extensions") - 1,
                                    &ext_len);
    if (ext == NULL) {
        return 0;
    }

    /* Walk the comma-separated offer list. Each offer is "name; params". */
    size_t i = 0;
    while (i < ext_len) {
        const size_t offer_start = i;
        while (i < ext_len && ext[i] != ',') i++;
        const size_t offer_end = i;
        if (i < ext_len) i++;   /* consume comma */

        size_t p          = offer_start;
        size_t name_start = p;
        while (p < offer_end && ext[p] != ';') p++;
        size_t name_end = p;
        while (name_start < name_end &&
               (ext[name_start] == ' ' || ext[name_start] == '\t')) name_start++;
        while (name_end > name_start &&
               (ext[name_end - 1] == ' ' || ext[name_end - 1] == '\t')) name_end--;

        if (name_end - name_start != sizeof("permessage-deflate") - 1 ||
            strncasecmp(ext + name_start, "permessage-deflate",
                        sizeof("permessage-deflate") - 1) != 0) {
            continue;
        }

        /* Matched. A pinned server_max_window_bits is honoured by running
         * our deflater at that window (RFC 7692 §7.1.2.1); only bits our
         * zlib cannot produce (raw deflate minimum is 9) decline this
         * offer — the walk then tries the client's next fallback offer. */
        bool acceptable = true;
        int  bits = 15;
        size_t q = p;
        while (q < offer_end) {
            if (ext[q] == ';') q++;
            while (q < offer_end && (ext[q] == ' ' || ext[q] == '\t')) q++;
            const size_t ps = q;
            while (q < offer_end && ext[q] != ';' && ext[q] != '=') q++;
            size_t pe = q;
            while (pe > ps && (ext[pe - 1] == ' ' || ext[pe - 1] == '\t')) pe--;

            long val = -1;
            if (q < offer_end && ext[q] == '=') {
                q++;
                while (q < offer_end &&
                       (ext[q] == ' ' || ext[q] == '\t' || ext[q] == '"')) q++;
                bool have = false;
                val = 0;
                while (q < offer_end && ext[q] >= '0' && ext[q] <= '9') {
                    val = val * 10 + (ext[q] - '0');
                    q++;
                    have = true;
                }
                if (!have) val = -1;
                while (q < offer_end && ext[q] != ';') q++;
            }

            if (pe - ps == sizeof("server_max_window_bits") - 1 &&
                strncasecmp(ext + ps, "server_max_window_bits",
                            sizeof("server_max_window_bits") - 1) == 0 &&
                val >= 0) {
                if (val >= 9 && val <= 15) {
                    if ((int)val < bits) {
                        bits = (int)val;
                    }
                } else {
                    /* 8 (zlib can't) or out-of-range — decline this offer. */
                    acceptable = false;
                    break;
                }
            }
        }

        if (acceptable) {
            return bits;
        }
    }

    return 0;
}
