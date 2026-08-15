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
 * the high-watermark for longer than write_timeout_ms — a slow consumer —
 * and by publish() when the connection is over its configured
 * setWsPublishRateLimit(). Either way the connection stays up: catching
 * this is the application's signal to drop the message, back off, or close.
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

/**
 * Reliable room delivery ({@see Room::send()} / {@see HttpServer::send()})
 * failed: the retry deadline passed with a target mailbox still full, the
 * outbound queue was at its cap, or send() was called outside a coroutine.
 *
 * DISTINCT from {@see WebSocketBackpressureException} (a rate-limiter trip where
 * nothing was sent): a delivery failure is at-least-once with partial delivery —
 * the fast targets were posted during fan-out, before any failure verdict. The
 * counts say how much landed, so re-sending (which duplicates on the ones that
 * already got it) is a decision, not an accident.
 *
 * @strict-properties
 */
final class RoomDeliveryException extends WebSocketException
{
    /** Worker mailboxes that had accepted the message when the send failed. */
    public readonly int $delivered;

    /** Targets still unfilled when the send gave up. */
    public readonly int $pending;
}
