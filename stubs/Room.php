<?php

/**
 * @generate-class-entries
 */

namespace TrueAsync;

/**
 * A server-side handle to a room (a pub/sub topic), obtained from
 * {@see HttpServer::room()}.
 *
 * Publishing through it reaches every subscriber of the topic across all
 * workers — WebSocket connections that called {@see WebSocket::subscribe()} —
 * with no sending connection, so nobody is excluded. Unlike
 * {@see WebSocket::publish()} it needs no connection, so a background producer
 * (a coroutine that is not a socket) can push into a room.
 *
 * @strict-properties
 * @not-serializable
 */
final class Room
{
    /* Rooms are minted by HttpServer::room(), never with `new`. */
    private function __construct() {}

    /**
     * Publish a text message to this room.
     *
     * @return int Subscribers served on the calling worker. Delivery to other
     *             workers is asynchronous, so this is a local count, not a total.
     */
    public function publish(string $message): int {}

    /**
     * Publish a binary message to this room.
     *
     * @return int Subscribers served on the calling worker.
     */
    public function publishBinary(string $data): int {}

    /**
     * Count the subscribers of this room across all workers (scatter/gather).
     *
     * Suspends the calling coroutine until every worker answers or $timeoutMs
     * elapses. Must run on a worker thread (a request/WebSocket handler or a
     * spawned run coroutine); on the pool parent it returns the local count.
     */
    public function subscriberCount(int $timeoutMs = 1000): int {}

    /** This room's topic name. */
    public function name(): string {}
}
