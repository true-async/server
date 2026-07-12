<?php

/**
 * @generate-class-entries
 * @strict-properties
 * @not-serializable
 */

namespace TrueAsync;

/**
 * A named room whose members may live in different workers.
 *
 * A worker is a thread with its own PHP context, so a plain PHP array of
 * connections can only ever reach the peers of one worker. The room is held by
 * the server instead: membership is registered in C, and a broadcast is handed
 * to each worker that holds members, which then writes to its own sockets.
 *
 * Obtain one with HttpServer::room(); the same name always yields the same room
 * across every worker of the process.
 *
 * Concurrency
 * -----------
 * - join()/leave() take a WebSocket owned by the CALLING worker.
 * - broadcast() may be called from any worker and any coroutine. It never
 *   suspends: a peer whose outbound queue is backed up drops the message rather
 *   than stalling delivery to the rest of the room (trySend semantics). Use
 *   WebSocket::send() on a single connection when you need delivery guarantees.
 * - Ordering is per connection (a room's messages reach one peer in the order
 *   they were broadcast); there is no global order across workers.
 * - A connection leaves every room automatically when it closes.
 */
final class WebSocketRoom
{
    /**
     * Instances are constructed internally — use HttpServer::room().
     */
    private function __construct() {}

    /**
     * Add a connection owned by this worker to the room. Idempotent.
     */
    public function join(WebSocket $ws): void {}

    /**
     * Remove a connection from the room. Idempotent — leaving a room the
     * connection never joined is a no-op.
     */
    public function leave(WebSocket $ws): void {}

    /**
     * Fan a text message out to every member, in every worker.
     *
     * @param string $text UTF-8, as for WebSocket::send().
     * @param WebSocket|null $except Skip this member — the usual "everyone but
     *        the sender" case in a chat room.
     * @return int Members the message was addressed to (a snapshot: membership
     *         can change while the fan-out is in flight).
     */
    public function broadcast(string $text, ?WebSocket $except = null): int {}

    /**
     * Binary counterpart of broadcast().
     */
    public function broadcastBinary(string $data, ?WebSocket $except = null): int {}

    /**
     * Members across all workers.
     */
    public function count(): int {}

    /**
     * The name this room was obtained with.
     */
    public function getName(): string {}
}
