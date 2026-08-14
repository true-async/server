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
 * A handle owns a reference to the topic hub, so it keeps publishing after the
 * {@see HttpServer} that minted it is released.
 *
 * @strict-properties
 * @not-serializable
 */
final class Room
{
    /* Rooms are minted by HttpServer::room(), never with `new`. */
    private function __construct() {}

    /**
     * Publish a text message to this room (best-effort, no retry).
     *
     * @return array{served: int, posted: int, dropped: int} Per-call delivery
     *         breakdown: `served` local subscribers on the calling worker,
     *         `posted` remote worker mailboxes that accepted the copy, `dropped`
     *         full remote mailboxes that lost it. Delivery to other workers is
     *         asynchronous, so `served` is a local count, not a total.
     */
    public function publish(string $message): array {}

    /**
     * Publish a binary message to this room.
     *
     * @return int Subscribers served on the calling worker.
     */
    public function publishBinary(string $data): int {}

    /**
     * Reliable send, NON-BLOCKING. Fans out now; for every target whose mailbox
     * is full, parks a retry entry on this worker's outbound queue and returns at
     * once — a background drainer retries it up to the deadline. Unlike
     * {@see publish()}, nothing is silently dropped, and the caller gets an
     * immediate, honest answer.
     *
     * @param int|null $timeoutMs How long the background drainer keeps retrying a
     *        still-full target. Null uses
     *        {@see HttpServerConfig::setWsPublishRetryTimeoutMs()}.
     * @return bool True if delivered outright or parked for retry; false if the
     *         outbound queue is at {@see HttpServerConfig::setWsPublishRetryQueueMax()}
     *         and nothing was parked — the caller must handle this now. The
     *         eventual outcome of a parked message is in {@see HttpServer::getRuntimeStats()}.
     */
    public function trySend(string $message, ?int $timeoutMs = null): bool {}

    /**
     * Reliable send, BLOCKING. Same fan-out-and-park as {@see trySend()}, but the
     * calling coroutine awaits the parked message's completion: it returns the
     * number of targets delivered to once every target lands, or THROWS if the
     * deadline passes with a target still full (or the queue was full at enqueue).
     * The caller either knows it landed or catches the failure.
     *
     * A target that detached while we waited — its slot reused by a fresh worker —
     * is skipped rather than mis-delivered, and does not by itself fail the send.
     *
     * Must run in a coroutine (it suspends). Because it blocks, use it for
     * point-to-point coordination, NOT for a fan-out to many dashboards — that is
     * what {@see publish()} is for.
     *
     * On failure the message may already have reached a subset of targets (the
     * fast ones are posted during fan-out, before any verdict); the thrown
     * exception carries how many landed, so re-sending — which duplicates on those
     * — is a decision, not an accident.
     *
     * @param int|null $timeoutMs Retry deadline; null uses
     *        {@see HttpServerConfig::setWsPublishRetryTimeoutMs()}.
     * @return int Targets the message was delivered to.
     * @throws RoomDeliveryException if the deadline passed with a target still
     *         full, or the outbound queue was full at enqueue, or send() was
     *         called outside a coroutine (use trySend() there).
     */
    public function send(string $message, ?int $timeoutMs = null): int {}

    /**
     * Count the subscribers of this room across all workers (scatter/gather).
     *
     * Suspends the calling coroutine until every worker answers or $timeoutMs
     * elapses. Must run on a worker thread (a request/WebSocket handler or a
     * spawned run coroutine); on the pool parent it returns the local count.
     *
     * A thread that never attached to the hub — a ThreadPool task the room was
     * transferred into — gets 0, which reads the same as a room nobody joined.
     * {@see trySend()} and {@see send()} report that thread honestly; this does not.
     */
    public function subscriberCount(int $timeoutMs = 1000): int {}

    /** This room's topic name. */
    public function name(): string {}
}
