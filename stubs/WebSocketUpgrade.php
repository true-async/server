<?php

/**
 * @generate-class-entries
 * @strict-properties
 * @not-serializable
 */

namespace TrueAsync;

/**
 * Handle on the in-progress WebSocket upgrade — exists from the
 * moment the handler is invoked until either reject() is called or
 * the handler returns successfully (in which case the 101 is
 * dispatched with whatever subprotocol setSubprotocol selected).
 *
 * Surface only available to handlers that declare a third
 * parameter:
 *
 *   addWebSocketHandler(function (WebSocket $ws, HttpRequest $req,
 *                                 WebSocketUpgrade $u): void { ... });
 *
 * The handler is always called with three arguments and PHP drops
 * the ones it does not declare, so a shorter handler simply never
 * sees this object and the upgrade is accepted with default
 * settings.
 *
 * Once the handshake commits, calls on this object throw —
 * subprotocol can no longer change once Sec-WebSocket-Protocol has
 * been written to the wire.
 */
final class WebSocketUpgrade
{
    private function __construct() {}

    /**
     * Reject the upgrade with the given HTTP status. The 101 will not
     * be sent; the connection responds with the chosen status and
     * closes. After reject() the handler should return — no further
     * I/O is permitted.
     *
     * @param int $status HTTP status code (must be 4xx or 5xx)
     * @param string $reason Optional response body
     */
    public function reject(int $status, string $reason = ''): void {}

    /**
     * Pick a subprotocol from the client's offered list. The selected
     * token will be echoed in Sec-WebSocket-Protocol. Must be called
     * before the handler returns and before reject(). The token is
     * not re-validated against `getOfferedSubprotocols()` — the caller
     * is responsible for choosing a valid offer.
     */
    public function setSubprotocol(string $name): void {}

    /**
     * @return string[] Tokens parsed from Sec-WebSocket-Protocol on the
     * incoming request, in client-preferred order. Empty when the
     * client did not offer any.
     */
    public function getOfferedSubprotocols(): array {}

    /**
     * @return string[] Raw extension offers from Sec-WebSocket-Extensions,
     * in client-preferred order. permessage-deflate (RFC 7692) is
     * negotiated automatically when enabled via
     * HttpServerConfig::setWsPermessageDeflate(); the remaining offers are
     * informational. Empty when the client did not offer any.
     */
    public function getOfferedExtensions(): array {}
}
