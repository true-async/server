<?php

/**
 * @generate-class-entries
 * @strict-properties
 * @not-serializable
 */

namespace TrueAsync;

/**
 * One WebSocket connection. Created by the server immediately after
 * the upgrade handshake commits and passed as the first argument to
 * the handler registered via HttpServer::addWebSocketHandler().
 *
 * Lifecycle
 * ---------
 * The connection is bound to the handler coroutine. When the handler
 * returns — for any reason, including `return` from the recv loop on
 * a null result — the server closes the connection with 1000 Normal.
 * Explicit close() before return is supported when a non-default code
 * or reason is required.
 *
 * Concurrency model
 * -----------------
 * - send() / sendBinary() / ping() are safe to call from any coroutine
 *   on the same thread. Producers enqueue serialized frames atomically;
 *   a single cooperative flusher writes them to the socket one frame
 *   at a time, so frames cannot interleave on the wire.
 * - recv() is single-reader. A second concurrent recv() throws
 *   WebSocketConcurrentReadException — the wire is one byte stream
 *   and there is no defined semantics for multiple readers.
 * - close() is idempotent and can be called from any coroutine.
 */
final class WebSocket implements \Iterator
{
    /**
     * Instances are constructed internally by the server.
     */
    private function __construct() {}

    /**
     * Receive the next text or binary message. Suspends the calling
     * coroutine until a complete message arrives or the connection
     * closes.
     *
     * @return WebSocketMessage|null A message, or null when the peer
     * closed cleanly — a normal CLOSE code (1000/1001/1005) or a plain
     * disconnect with no CLOSE frame. Loops typically
     * `while (($m = $ws->recv()) !== null) { ... }`.
     *
     * @throws WebSocketClosedException on a protocol error or an explicit
     *         error close code; the exception's readonly $closeCode /
     *         $closeReason carry the RFC 6455 code and reason text.
     * @throws WebSocketConcurrentReadException if another coroutine is
     *         already blocked in recv() on this connection.
     */
    public function recv(): ?WebSocketMessage {}

    /**
     * Send a text frame. The data MUST be valid UTF-8 — invalid UTF-8
     * is rejected at the boundary so the receiver never sees a frame
     * that violates RFC 6455 §5.6.
     *
     * Returns immediately when the outbound queue is below the
     * high-watermark (the common case). Suspends the calling
     * coroutine when the queue is over the watermark and resumes once
     * drain brings it back below. Throws
     * WebSocketBackpressureException when the suspension exceeds
     * write_timeout_ms — the handler can then drop the message,
     * close, or retry.
     *
     * @throws WebSocketBackpressureException on prolonged drain stall
     * @throws WebSocketClosedException if the connection is already closed
     */
    public function send(string $text): void {}

    /**
     * Send a binary frame. Binary payloads have no UTF-8 constraint.
     *
     * @see send() for backpressure semantics — they are identical.
     */
    public function sendBinary(string $data): void {}

    /**
     * Non-blocking send. Queues a text frame and returns true when the
     * outbound queue is below the high-water mark; returns false WITHOUT
     * queueing when it is at/over the mark, so the caller can drop the
     * message, slow down, or close. Never suspends the calling coroutine —
     * the right tool for a broadcast loop where one slow client must not
     * stall delivery to the others.
     *
     * The high-water mark is HttpServerConfig::setStreamWriteBufferBytes()
     * (0 = disabled → trySend always queues and returns true).
     *
     * @return bool true if accepted, false if backpressured (BUSY).
     * @throws WebSocketClosedException if the connection is already closed.
     */
    public function trySend(string $text): bool {}

    /**
     * Non-blocking binary send. @see trySend() for the semantics.
     *
     * @return bool true if accepted, false if backpressured (BUSY).
     * @throws WebSocketClosedException if the connection is already closed.
     */
    public function trySendBinary(string $data): bool {}

    /**
     * Send a PING frame. The peer is required by RFC 6455 §5.5.2 to
     * reply with a PONG. Application code rarely needs to call this —
     * the server's keepalive timer (HttpServerConfig::setWsPingIntervalMs)
     * sends pings automatically when configured.
     *
     * @param string $payload Up to 125 bytes (RFC 6455 §5.5).
     */
    public function ping(string $payload = ''): void {}

    /**
     * Initiate the close handshake and tear the connection down.
     * Idempotent — subsequent calls are no-ops.
     *
     * @param WebSocketCloseCode|int $code Standard code via the enum,
     *        or a raw integer in 4000-4999 (application-specific
     *        codes per RFC 6455 §7.4.2).
     * @param string $reason UTF-8 reason text, up to 123 bytes (close
     *        frame payload is 125 bytes minus 2 for the code).
     */
    public function close(WebSocketCloseCode|int $code = WebSocketCloseCode::NORMAL,
                          string $reason = ''): void {}

    /**
     * True after close() has been called or the peer's CLOSE frame
     * has been processed.
     */
    public function isClosed(): bool {}

    /**
     * The subprotocol negotiated during the upgrade, or null if none
     * was selected.
     */
    public function getSubprotocol(): ?string {}

    /**
     * Client IP address of the socket peer, e.g. "203.0.113.7" or "2001:db8::1".
     *
     * Bare IP only: no port, and no brackets around an IPv6 literal — the same
     * shape as $_SERVER['REMOTE_ADDR'] and HttpRequest::getRemoteAddress().
     * Use getRemotePort() for the port.
     *
     * NULL on a Unix-socket listener, which has no IP peer.
     */
    public function getRemoteAddress(): ?string {}

    /**
     * Client port of the socket peer, e.g. 54321. NULL when there is no IP peer.
     */
    public function getRemotePort(): ?int {}

    // === Topics (issue #2) — publish/subscribe across every worker.
    //
    // A worker is a thread with its own PHP context, so an array of connections
    // could only ever reach the peers of one worker. Topics live in the server:
    // each worker indexes the connections it owns, and a publish is handed to
    // every worker, which delivers to its own sockets. No Redis, no
    // setWorkers(1).
    //
    // A topic is addressed by NAME, at the call site — there is no topic object
    // to obtain, hold, or pass around.
    //
    // Filters follow MQTT: levels are separated by `/`, `+` matches exactly one
    // level, and a trailing `#` matches zero or more. So `user/42/#` receives
    // `user/42/presence` and `user/42` alike, and `order/+/status` receives the
    // status of an order that did not exist when you subscribed.

    /**
     * Subscribe this connection to a topic filter. Idempotent.
     *
     * @param string $filter Topic filter; may contain `+` / `#` wildcards.
     * @throws WebSocketException on a malformed filter, or once the connection
     *         already holds its HttpServerConfig::setWsMaxSubscriptions() limit.
     */
    public function subscribe(string $filter): void {}

    /**
     * Unsubscribe from a filter. Idempotent — a filter never subscribed to is a
     * no-op. A closing connection unsubscribes from everything by itself.
     */
    public function unsubscribe(string $filter): void {}

    /**
     * The filters this connection subscribed through, in no particular order.
     *
     * @return string[]
     */
    public function getTopics(): array {}

    /**
     * Publish a text message to a topic, on every worker.
     *
     * Never suspends: a peer whose outbound queue is backed up drops the message
     * rather than stalling delivery to the rest of the topic (trySend
     * semantics). Use send() on a single connection when you need delivery
     * guarantees.
     *
     * A subscriber matched by several of its own filters still receives one
     * copy.
     *
     * @param string $topic Concrete topic — wildcards are rejected, because a
     *        message fanned out to a pattern has no well-defined destination.
     * @param bool $excludeSelf Skip this connection — the "everyone but the
     *        sender" case that a chat wants.
     * @return int Subscribers served on the CALLING worker. Delivery to the
     *         other workers is asynchronous and cannot be counted here, so this
     *         is a local number, not a process-wide one.
     * @throws WebSocketException on a malformed topic, or one carrying a wildcard.
     * @throws WebSocketBackpressureException when the connection is over its
     *         HttpServerConfig::setWsPublishRateLimit(). The connection stays up.
     */
    public function publish(string $topic, string $text, bool $excludeSelf = true): int {}

    /**
     * Binary counterpart of publish().
     */
    public function publishBinary(string $topic, string $data, bool $excludeSelf = true): int {}

    /**
     * Connections across all workers that a publish to $topic would reach —
     * including those subscribed through a wildcard that matches it.
     *
     * Each worker answers with its own count and the answers are summed, so this
     * is a snapshot rather than a live number: a worker that does not answer in
     * time is left out.
     */
    public function subscriberCount(string $topic): int {}

    // === Iterator === so `foreach ($ws as $msg)` mirrors a recv() loop.
    // The cursor advances by pulling the next message; iteration ends on a
    // graceful close and throws WebSocketClosedException on an error close.
    public function current(): ?WebSocketMessage {}
    public function key(): int {}
    public function next(): void {}
    public function rewind(): void {}
    public function valid(): bool {}
}
