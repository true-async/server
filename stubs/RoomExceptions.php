<?php

/**
 * @generate-class-entries
 */

namespace TrueAsync;

/**
 * Reliable room delivery ({@see Room::send()} / {@see HttpServer::send()})
 * failed: the retry deadline passed with a target mailbox still full, the
 * outbound queue was at its cap, or send() was called outside a coroutine.
 *
 * Delivery is at-least-once with partial delivery — the fast targets were posted
 * during fan-out, before any failure verdict. The counts say how much landed, so
 * re-sending (which duplicates on the ones that already got it) is a decision,
 * not an accident.
 *
 * Extends HttpServerException rather than WebSocketException: rooms are served
 * by a build configured with --disable-websocket, where that class does not
 * exist. A handler that caught WebSocketException around a send() catches
 * HttpServerException instead.
 *
 * @strict-properties
 */
final class RoomDeliveryException extends HttpServerException
{
    /** Worker mailboxes that had accepted the message when the send failed. */
    public readonly int $delivered;

    /** Targets still unfilled when the send gave up. */
    public readonly int $pending;
}
