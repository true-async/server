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
     * @return bool True if stopped successfully
     */
    public function stop(): bool {}

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
     *
     * @return array
     */
    public function getRuntimeStats(): array {}
}
