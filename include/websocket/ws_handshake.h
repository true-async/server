/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

#ifndef WS_HANDSHAKE_H
#define WS_HANDSHAKE_H

#include "php.h"
#include "php_http_server.h"

typedef struct http_request_t http_request_t;

/*
 * RFC 6455 §1.3 magic GUID concatenated to Sec-WebSocket-Key before
 * SHA-1 to produce Sec-WebSocket-Accept. Constant per spec — never
 * vary between versions.
 */
#define WS_MAGIC_GUID  "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

/*
 * Sec-WebSocket-Key wire length: base64 encoding of 16 random bytes
 * is exactly 24 characters. Anything else is rejected by validation.
 */
#define WS_CLIENT_KEY_LEN 24

/*
 * Sec-WebSocket-Accept wire length: base64 encoding of 20-byte SHA-1
 * digest is exactly 28 characters. Used to size the static response
 * buffer.
 */
#define WS_ACCEPT_LEN 28

typedef enum {
    WS_HANDSHAKE_NOT_AN_UPGRADE = 0,  /* not a WS request — caller does normal H1 dispatch */
    WS_HANDSHAKE_OK             = 1,  /* validated; caller may compute Accept and emit 101 */
    /* Negative codes: validation failed, caller emits the corresponding HTTP status */
    WS_HANDSHAKE_BAD_REQUEST    = -400,
    WS_HANDSHAKE_FORBIDDEN_METHOD = -405,  /* methods other than GET */
    WS_HANDSHAKE_UPGRADE_REQUIRED = -426,  /* Sec-WebSocket-Version != 13 */
} ws_handshake_result_t;

/*
 * Decide whether `req` is a valid WebSocket upgrade request per
 * RFC 6455 §4.1. Reads Connection / Upgrade / Sec-WebSocket-Version /
 * Sec-WebSocket-Key headers and the request method.
 *
 * Returns one of ws_handshake_result_t. Read-only — does not mutate
 * `req`.
 *
 * The lookup is cheap enough to run on every H1 request and the
 * common case (no Upgrade header) returns WS_HANDSHAKE_NOT_AN_UPGRADE
 * after a single hash probe.
 */
ws_handshake_result_t ws_handshake_validate(const http_request_t *req);

/*
 * Compute Sec-WebSocket-Accept = base64(SHA-1(client_key || MAGIC_GUID))
 * per RFC 6455 §4.2.2. `client_key` is the raw 24-byte value from the
 * client's Sec-WebSocket-Key header; `out` receives 28 chars (no NUL,
 * no trailing CRLF). Caller is responsible for sizing `out` to at least
 * WS_ACCEPT_LEN bytes.
 *
 * Returns 0 on success, -1 if `client_key_len` is not WS_CLIENT_KEY_LEN.
 */
int ws_handshake_compute_accept(const char *client_key, size_t client_key_len,
                                char *out);

/*
 * Render the 101 Switching Protocols response into a freshly-allocated
 * zend_string ready to be handed to http_connection_send.
 *
 * Includes mandatory headers (Upgrade, Connection, Sec-WebSocket-Accept)
 * and, if `subprotocol` is non-NULL, Sec-WebSocket-Protocol. When
 * `deflate_bits` > 0, advertises the negotiated permessage-deflate
 * extension (RFC 7692) with server+client no_context_takeover; a value
 * below 15 additionally echoes `server_max_window_bits=N`. The
 * `accept` argument is the 28-char output of ws_handshake_compute_accept.
 *
 * Returns NULL on allocation failure. Caller owns the returned string
 * and must release it with zend_string_release.
 */
zend_string *ws_handshake_build_101_response(const char *accept,
                                             const char *subprotocol,
                                             int deflate_bits);

/*
 * Negotiate permessage-deflate (RFC 7692) against the request's
 * Sec-WebSocket-Extensions offer list. Read-only. Returns the server
 * deflate window bits to use — 15 for a plain offer, 9..14 when the
 * offer pins server_max_window_bits (we honour it via deflateInit2) —
 * or 0 to decline (header absent, extension not offered, or every
 * offer demands bits our zlib cannot produce: raw deflate min is 9).
 */
int ws_pmce_negotiate(const http_request_t *req);

#endif /* WS_HANDSHAKE_H */
