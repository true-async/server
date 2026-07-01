<?php

/**
 * @generate-class-entries
 */

namespace TrueAsync;

/**
 * Base class for all WebSocket exceptions. Extends the project-wide
 * HttpServerException so existing catch-all handlers keep working.
 * @strict-properties
 */
class WebSocketException extends HttpServerException
{
}

/**
 * The connection has been closed for a reason other than a normal
 * peer-initiated handshake. `code` carries the RFC 6455 close code
 * (or 1006 Abnormal Closure when no CLOSE frame was received);
 * `reason` is the UTF-8 reason text from the peer's CLOSE payload,
 * or empty when none was provided.
 *
 * Graceful close (peer-initiated CLOSE 1000) is signalled by
 * WebSocket::recv() returning null instead of throwing.
 */
final class WebSocketClosedException extends WebSocketException
{
    public readonly int $closeCode;
    public readonly string $closeReason;
}

/**
 * Raised by send() / sendBinary() when the outbound queue stays over
 * the high-watermark for longer than write_timeout_ms. Catching this
 * is the application's signal to either close the connection (slow
 * consumer detected) or drop the message and continue.
 */
final class WebSocketBackpressureException extends WebSocketException
{
}

/**
 * Programmer error: a second coroutine called recv() while another
 * was already suspended in recv() on the same WebSocket. There is no
 * defined semantics for multiple readers on a single byte stream, so
 * this is rejected at the boundary instead of producing race-prone
 * behavior. Restructure to a single recv loop that dispatches.
 */
final class WebSocketConcurrentReadException extends WebSocketException
{
}
