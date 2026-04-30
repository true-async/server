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
     * Add HTTP/1.1 request handler
     *
     * Handler signature: function(HttpRequest $request, HttpResponse $response): void
     *
     * @param callable $handler Request handler callback
     * @return static
     */
    public function addHttpHandler(callable $handler): static {}

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
}
