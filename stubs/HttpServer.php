<?php

/**
 * @generate-class-entries
 */

namespace TrueAsync;

/**
 * HTTP Server
 * @strict-properties
 * @not-serializable
 */
final class HttpServer
{
    /**
     * Create HTTP server with configuration
     *
     * @param HttpServerConfig $config Server configuration
     */
    public function __construct(HttpServerConfig $config) {}

    /**
     * Whether the extension was built with HTTP/2 support (--enable-http2).
     *
     * Compile-time flag. Lets phpt SKIPIF blocks tell HTTP/2-only tests
     * to bail when the build omitted nghttp2.
     */
    public static function isHttp2(): bool {}

    /**
     * Whether the extension was built with HTTP/3 support (--enable-http3).
     *
     * Compile-time flag. Lets phpt SKIPIF blocks tell HTTP/3-only tests
     * to bail when the build omitted ngtcp2/nghttp3.
     */
    public static function isHttp3(): bool {}

    /**
     * Add HTTP/1.1 request handler
     *
     * Handler signature: function(HttpRequest $request, HttpResponse $response): void
     *
     * @param callable $handler Request handler callback
     * @return static
     */
    public function addHttpHandler(callable $handler): static {}

    /**
     * Register a built-in static file handler (issue #13).
     *
     * Matches URLs whose path begins with the handler's configured
     * prefix and serves files from its root directory entirely in C —
     * no PHP coroutine, no callback. Multiple calls are allowed; mounts
     * are matched in registration order. The supplied {@see StaticHandler}
     * is locked at attach time, so any subsequent setter call on it
     * throws HttpServerRuntimeException.
     *
     * @return static
     */
    public function addStaticHandler(StaticHandler $handler): static {}

    /**
     * Add WebSocket handler (TODO)
     *
     * @param callable $handler WebSocket handler callback
     * @return static
     */
    public function addWebSocketHandler(callable $handler): static {}

    /**
     * Enable cross-worker rooms (pub/sub topics) on this server.
     *
     * A room is a topic that any code can publish to — the message fans out to
     * every subscriber across all workers, over the same engine WebSocket
     * connections use with {@see WebSocket::subscribe()}. Registering a
     * WebSocket handler already allocates the room hub, so call this only when
     * you want to publish rooms without a WebSocket handler, or to make the
     * opt-in explicit. Must be called before start().
     *
     * @return static
     */
    public function enableRooms(): static {}

    /**
     * Publish a text message to a room, from the server side — no WebSocket
     * connection required.
     *
     * Reaches every subscriber of $topic on every worker. Unlike
     * {@see WebSocket::publish()} there is no sending connection, so nobody is
     * excluded. $topic must be a concrete name (no `+` or `#` wildcards).
     *
     * @return array{served: int, posted: int, dropped: int} Per-call delivery
     *         breakdown: `served` local subscribers on the calling worker,
     *         `posted` remote worker mailboxes that accepted the copy, `dropped`
     *         full remote mailboxes that lost it (best-effort — use send() /
     *         trySend() when a full mailbox must be retried instead of dropped).
     */
    public function publish(string $topic, string $message, bool $binary = false): array {}

    /**
     * Reliable server-side send to a room, NON-BLOCKING. Parks a full target on
     * the outbound queue for background retry and returns at once. See
     * {@see Room::trySend()} for the full contract; this is the same without a
     * {@see Room} handle.
     *
     * @param int|null $timeoutMs Retry deadline; null uses the configured default.
     * @return bool True if delivered or parked; false if the outbound queue is full.
     */
    public function trySend(string $topic, string $message, ?int $timeoutMs = null): bool {}

    /**
     * Reliable server-side send to a room, BLOCKING. Suspends the calling
     * coroutine until delivered or the deadline passes, then THROWS. See
     * {@see Room::send()} for the full contract; this is the same without holding
     * a {@see Room} handle.
     *
     * @param int|null $timeoutMs Retry deadline; null uses the configured default.
     * @return int Worker mailboxes that accepted the message. Local subscribers
     *             on the calling worker are served first and are NOT counted here;
     *             a mailbox that accepted it can still drop it on a full ring
     *             (`ws_sub_overflow`).
     * @throws RoomDeliveryException if the deadline passed with a target still
     *         full, the outbound queue was full, or called outside a coroutine.
     */
    public function send(string $topic, string $message, ?int $timeoutMs = null): int {}

    /**
     * Count the subscribers of a room across all workers (scatter/gather).
     *
     * Suspends the calling coroutine until every worker answers or $timeoutMs
     * elapses; a worker that misses the deadline is left out of the sum. Called
     * outside a coroutine it returns only the calling worker's count.
     */
    public function subscriberCount(string $topic, int $timeoutMs = 1000): int {}

    /**
     * Get a server-side handle to a room (topic), for publishing or counting
     * from outside a WebSocket connection.
     *
     * $topic must be a concrete name (no `+` or `#` wildcards). The returned
     * {@see Room} owns a reference to the topic hub, so it keeps publishing
     * after this server is released. Minting one before start() enables rooms.
     */
    public function room(string $topic): Room {}

    /**
     * Add HTTP/2 handler (TODO)
     *
     * @param callable $handler HTTP/2 handler callback
     * @return static
     */
    public function addHttp2Handler(callable $handler): static {}

    /**
     * Add gRPC handler (TODO)
     *
     * @param callable $handler gRPC handler callback
     * @return static
     */
    public function addGrpcHandler(callable $handler): static {}

    /**
     * Start server and begin accepting connections
     *
     * This method blocks until stop() is called or an error occurs.
     *
     * @return bool True if started successfully
     */
    public function start(): bool {}

    /**
     * Stop server gracefully
     *
     * Stops accepting new connections, waits for active requests to complete
     * (up to shutdown timeout), then closes all connections.
     *
     * On a pool parent (setWorkers(N), N > 1) this retires every worker and
     * suspends until the pool is down (issue #117), so when it returns the
     * server is really gone and start() has come back. Call it from a coroutine
     * — a SIGTERM/SIGINT handler is the usual place. On a standalone server it
     * does not suspend: it is normally called from a request handler, which the
     * shutdown drain itself waits on.
     *
     * @return bool True if stopped successfully
     */
    public function stop(): bool {}


    /**
     * Hot-reload the worker pool (issue #93). Pool parent only: workers finish
     * their in-flight work, stop and exit; fresh worker threads re-run the
     * bootloader — picking up changed code — and take over on the same listen
     * sockets. Suspends until the old cohort has drained. Call from a coroutine
     * on the pool parent (start() keeps running throughout); invalidate changed
     * files first (opcache_invalidate) or rely on opcache timestamp validation.
     *
     * @return bool True when every replacement worker was resubmitted; false if
     *              a reload is already in progress or some replacement failed.
     */
    public function reload(): bool {}

    /**
     * Check if server is running
     */
    public function isRunning(): bool {}

    /**
     * Get server telemetry (TODO)
     *
     * @return array Telemetry data
     */
    public function getTelemetry(): array {}

    /**
     * Reset telemetry counters (TODO)
     *
     * @return bool True if reset successfully
     */
    public function resetTelemetry(): bool {}

    /**
     * Get server configuration
     *
     * @return HttpServerConfig The configuration object
     */
    public function getConfig(): HttpServerConfig {}

    /**
     * Get per-listener HTTP/3 observability counters.
     *
     * One entry per addHttp3Listener() in order. Each entry carries host,
     * port, datagrams_received, bytes_received, datagrams_errored,
     * last_datagram_size, last_peer. Returns an empty array when the
     * extension is built without --enable-http3.
     *
     * @return array
     */
    public function getHttp3Stats(): array {}

    /**
     * Snapshot of server-side arena/pool counters.
     *
     * Reports memory committed by the server's own internal allocators
     * (slab pools, per-thread caches) so a benchmark probe can attribute
     * RSS growth to a concrete subsystem.
     *
     *  - `conn_arena_live`     — http_connection_t slots currently in
     *                            use (one per live TCP connection).
     *  - `conn_arena_slots`    — total slots across all chunks (live +
     *                            free, never shrinks).
     *  - `conn_arena_chunks`   — slab chunks committed. Each chunk
     *                            holds CONN_ARENA_CHUNK_SLOTS (256)
     *                            http_connection_t structs (~768 B each).
     *  - `conn_arena_bytes`    — `chunks * CONN_ARENA_CHUNK_SLOTS *
     *                            sizeof(http_connection_t)`, virtual
     *                            commitment.
     *  - `body_pool`           — per-size-class LIFO of large request
     *                            bodies (1 MB to 128 MB). Each entry has
     *                            `slot_bytes`, `count` (slots cached
     *                            right now), `bytes` (`count *
     *                            slot_bytes`).
     *  - `body_pool_total_bytes` — sum of `bytes` across all classes.
     *  - `ws_topic_posted`      — cross-worker publishes handed to another
     *                             worker's mailbox.
     *  - `ws_topic_skipped`     — workers a publish did NOT wake, because the
     *                             interest filter proved they hold no
     *                             subscriber. Large next to `posted` means the
     *                             filter is earning its keep.
     *  - `ws_topic_dropped`     — publishes a worker's mailbox would not take
     *                             because it was full. This one is data loss:
     *                             a worker is not draining fast enough, or a
     *                             client is flooding publishes.
     *  - `ws_bodies`            — message bodies allocated. One publish costs
     *                             one, whatever it reaches: the body is shared
     *                             by every subscriber and every worker it lands
     *                             in. Growing faster than the publishes means a
     *                             copy crept back into a delivery path.
     *  - `ws_sub_overflow`      — server-side receivers (see {@see Room::recv()})
     *                             whose 64-message ring dropped its oldest
     *                             because nobody was reading. Distinct from
     *                             `ws_topic_dropped`: that is the transport
     *                             giving up on a worker, this is a receiver too
     *                             slow for itself. {@see Room::lostCount()}
     *                             attributes it to one subscription.
     *  - `ws_retry_queued`, `ws_retry_delivered`, `ws_retry_expired`,
     *    `ws_retry_rejected`, `ws_retry_gone`, `ws_retry_shutdown` — the
     *                             reliable path's ledger: parked targets, those
     *                             a retry landed, those that ran out of
     *                             deadline, sends the outbound queue refused,
     *                             targets that had detached by the time a retry
     *                             came round, and those a worker shutdown
     *                             abandoned. See {@see Room::send()}.
     *
     * @return array
     */
    public function getRuntimeStats(): array {}

    /**
     * Cross-worker statistics aggregate (issue #5).
     *
     * Opt-in: throws a RuntimeException unless
     * {@see HttpServerConfig::setStatsEnabled()} was set to true. Returns:
     *
     *   [
     *     'enabled' => true,
     *     'workers' => [ <id> => ['total_requests'=>…, …], … ],
     *     'totals'  => ['total_requests'=>…, …],   // summed across workers
     *   ]
     *
     * Counters are read directly from each worker's slab slot with no locking,
     * so the aggregate may be stale by at most one worker mid-rotation. A
     * single-worker server reports its own counters as the sole worker.
     *
     * @return array
     */
    public function getStats(): array {}
}
