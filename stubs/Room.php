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
     * @return int Worker mailboxes that accepted the message. Local subscribers
     *             on the calling worker are served first and are NOT counted here;
     *             a mailbox that accepted it can still drop it on a full ring
     *             (`ws_sub_overflow`).
     * @throws RoomDeliveryException if the deadline passed with a target still
     *         full, or the outbound queue was full at enqueue, or send() was
     *         called outside a coroutine (use trySend() there).
     */
    public function send(string $message, ?int $timeoutMs = null): int {}

    /**
     * Join this room in the CALLING thread, so {@see recv()} can take messages
     * published to it — by another thread, another worker, or a WebSocket peer.
     *
     * Attaches this thread to the topic machinery if nobody did yet. Idempotent
     * per handle. A subscription belongs to one thread and is never carried by a
     * transfer: a room handed to a {@see \Async\ThreadPool} task arrives
     * unsubscribed, and the task subscribes for itself.
     */
    public function subscribe(): void {}

    /**
     * Leave this room in the calling thread. The thread stays attached — other
     * rooms of the same server go on receiving.
     */
    public function unsubscribe(): void {}

    /**
     * Take the next message, or wait for one.
     *
     * Returns null when $timeoutMs passes with nothing to take, or at once when
     * called outside a coroutine with nothing queued. Binary and text messages
     * both come back as a string — the WebSocket frame type says nothing to a
     * server-side consumer.
     *
     * @param int|null $timeoutMs Deadline in milliseconds. null waits without
     *        one; 0 (or a negative, which is what an expired computed deadline
     *        comes out as) takes whatever is already queued and returns.
     * @throws HttpServerRuntimeException if this thread never subscribed, if
     *         another coroutine is already parked on this room, or if the
     *         subscription closed while parked.
     */
    public function recv(?int $timeoutMs = null): ?string {}

    /**
     * Messages this room's queue dropped because it was full.
     *
     * Monotonic for the lifetime of this handle, across unsubscribe/subscribe:
     * snapshot it around a drain and compare. A publisher is never blocked by a
     * slow consumer, so a receiver that falls behind loses the oldest messages —
     * this is how it finds out. Whatever is still queued when a subscription
     * ends is discarded and does NOT count here.
     */
    public function lostCount(): int {}

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
