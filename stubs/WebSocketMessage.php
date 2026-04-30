<?php

/**
 * @generate-class-entries
 * @strict-properties
 * @not-serializable
 */

namespace TrueAsync;

/**
 * One fully-reassembled WebSocket message, as delivered by
 * WebSocket::recv(). Text messages have already been UTF-8 validated
 * by the framing layer — receivers can use `data` as-is without
 * re-checking.
 */
final class WebSocketMessage
{
    /**
     * Message payload. For text messages this is valid UTF-8.
     */
    public readonly string $data;

    /**
     * True if the message was sent as a Binary frame (opcode 0x2),
     * false if Text (opcode 0x1).
     */
    public readonly bool $binary;

    /**
     * Instances are constructed internally by the server. User code
     * receives them via WebSocket::recv() — never `new WebSocketMessage`.
     */
    private function __construct() {}
}
