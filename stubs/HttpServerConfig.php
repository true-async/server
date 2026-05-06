<?php

/**
 * @generate-class-entries
 */

namespace TrueAsync;

/**
 * HTTP Server configuration
 * @strict-properties
 * @not-serializable
 */
final class HttpServerConfig
{
    /**
     * Create server configuration
     *
     * @param string|null $host Default host for single listener (shortcut)
     * @param int $port Default port for single listener (shortcut)
     */
    public function __construct(?string $host = null, int $port = 8080) {}

    // === Listener configuration ===

    /**
     * Add TCP listener accepting both HTTP/1.1 and HTTP/2 (h2c via preface
     * detection on plaintext, h2 via ALPN on TLS). For protocol-restricted
     * ports use {@see addHttp1Listener()}, {@see addHttp2Listener()}, or
     * {@see addHttp3Listener()}.
     *
     * @param string $host Host to bind (e.g., "127.0.0.1", "0.0.0.0")
     * @param int $port Port to listen on
     * @param bool $tls Enable TLS for this listener
     * @return static
     */
    public function addListener(string $host, int $port, bool $tls = false): static {}

    /**
     * Add HTTP/1.1-only TCP listener.
     *
     * A connection that opens with the HTTP/2 preface is handed to llhttp,
     * which emits a compliant 400 Bad Request and closes.
     *
     * @param string $host Host to bind
     * @param int $port Port to listen on
     * @param bool $tls Enable TLS for this listener
     * @return static
     */
    public function addHttp1Listener(string $host, int $port, bool $tls = false): static {}

    /**
     * Add HTTP/2-only listener.
     *
     * With $tls=false this is h2c (cleartext HTTP/2): the listener requires
     * the RFC 7540 §3.5 preface and routes anything else into nghttp2's
     * BAD_CLIENT_MAGIC path, returning a compliant GOAWAY(PROTOCOL_ERROR).
     * With $tls=true the server only advertises h2 over ALPN.
     *
     * @param string $host Host to bind
     * @param int $port Port to listen on
     * @param bool $tls Enable TLS for this listener
     * @return static
     */
    public function addHttp2Listener(string $host, int $port, bool $tls = false): static {}

    /**
     * Add Unix socket listener (HTTP/1.1 + HTTP/2, h2c-style).
     *
     * @param string $path Path to Unix socket
     * @return static
     */
    public function addUnixListener(string $path): static {}

    /**
     * Add HTTP/3 (QUIC over UDP) listener
     *
     * QUIC mandates TLS 1.3, so the server's configured certificate / private
     * key are used automatically — no separate tls flag. Extension must be
     * built with --enable-http3; otherwise start() throws.
     *
     * @param string $host Host to bind (e.g. "0.0.0.0")
     * @param int $port UDP port
     * @return static
     */
    public function addHttp3Listener(string $host, int $port): static {}

    /**
     * Get all configured listeners
     *
     * @return array Array of listener configurations
     */
    public function getListeners(): array {}

    // === Connection limits ===

    /**
     * Set socket backlog (pending connections queue)
     *
     * @param int $backlog Backlog size (default: 128)
     * @return static
     */
    public function setBacklog(int $backlog): static {}

    /**
     * Built-in worker pool size (issue #11).
     *
     * 1 (default) = single-threaded. {@see HttpServer::start()} runs the
     * event loop on the calling thread, identical to pre-#11 behaviour.
     *
     * > 1 = `HttpServer::start()` spawns an `Async\ThreadPool` of this
     * size, replicates the config + handler set to each worker via
     * transfer_obj, and the parent's `start()` awaits all workers'
     * completion. Each worker re-binds the same listeners; the kernel
     * load-balances accept() across them via `SO_REUSEPORT` (Linux).
     *
     * @return static
     */
    public function setWorkers(int $workers): static {}

    /**
     * @return int Configured worker pool size (1 = single-threaded).
     */
    public function getWorkers(): int {}

    /**
     * Get socket backlog
     */
    public function getBacklog(): int {}

    /**
     * Set maximum concurrent connections
     *
     * @param int $maxConnections Max connections (0 = unlimited)
     * @return static
     */
    public function setMaxConnections(int $maxConnections): static {}

    /**
     * Get maximum connections
     */
    public function getMaxConnections(): int {}

    /**
     * Set maximum concurrent in-flight requests (overload shedding)
     *
     * Once this many handler coroutines are active, new requests get a
     * fast reject (H1 → 503 Service Unavailable with Retry-After: 1,
     * H2 → RST_STREAM REFUSED_STREAM which is retry-safe per
     * RFC 7540 §8.1.4). Keeps a single-worker event loop from
     * collapsing under c=100 m=10-style bursts on HTTP/2 — the hard
     * cap on *connections* does not stop new streams arriving on
     * already-accepted H2 connections.
     *
     * @param int $n Max in-flight requests; 0 = disabled (default).
     *               When 0 is left at start() the cap is derived from
     *               max_connections * 10.
     * @return static
     */
    public function setMaxInflightRequests(int $n): static {}

    /**
     * Get the admission-reject cap (0 = disabled).
     */
    public function getMaxInflightRequests(): int {}

    // === Timeouts ===

    /**
     * Set read timeout (for receiving request)
     *
     * @param int $timeout Timeout in seconds (0 = no timeout)
     * @return static
     */
    public function setReadTimeout(int $timeout): static {}

    /**
     * Get read timeout
     */
    public function getReadTimeout(): int {}

    /**
     * Set write timeout (for sending response)
     *
     * @param int $timeout Timeout in seconds (0 = no timeout)
     * @return static
     */
    public function setWriteTimeout(int $timeout): static {}

    /**
     * Get write timeout
     */
    public function getWriteTimeout(): int {}

    /**
     * Set keep-alive timeout (idle time between requests)
     *
     * @param int $timeout Timeout in seconds (0 = disable keep-alive)
     * @return static
     */
    public function setKeepAliveTimeout(int $timeout): static {}

    /**
     * Get keep-alive timeout
     */
    public function getKeepAliveTimeout(): int {}

    /**
     * Set shutdown timeout (graceful shutdown)
     *
     * @param int $timeout Timeout in seconds
     * @return static
     */
    public function setShutdownTimeout(int $timeout): static {}

    /**
     * Get shutdown timeout
     */
    public function getShutdownTimeout(): int {}

    // === Backpressure ===

    /**
     * Set CoDel target sojourn threshold for accept-side backpressure.
     *
     * Controls when the server pauses accepting new connections under load.
     * When per-request queue-wait (sojourn) stays above this threshold for
     * 100 ms, the listen socket is stopped until existing work drains.
     *
     * Guidance:
     *   - Fast handlers (<5 ms):  leave at default (5)
     *   - Typical web handlers:   10 – 20 ms
     *   - Slow handlers (DB, IO): 50 – 100 ms
     *   - 0 disables CoDel entirely (hard cap via setMaxConnections still
     *     applies if configured).
     *
     * See docs/BACKPRESSURE.md for the full mechanism.
     *
     * @param int $ms Target in milliseconds (0–10000). Default: 5.
     * @return static
     */
    public function setBackpressureTargetMs(int $ms): static {}

    /**
     * Get the configured CoDel target sojourn in milliseconds.
     */
    public function getBackpressureTargetMs(): int {}

    // === Connection draining (Step 8) ===
    //
    // Graceful-drain controls for load migration under overload. See
    // docs/PLAN_CONN_DRAIN.md for the full model. Proactive knobs (age/
    // grace) are opt-in and default to 0; reactive knobs (spread/
    // cooldown) have working defaults that fire on CoDel / hard-cap.

    /**
     * Set the proactive connection-age drain threshold (milliseconds).
     *
     * After (age ± 10% jitter) of lifetime, a connection is signalled
     * to close gracefully: HTTP/1 next response carries Connection:
     * close; HTTP/2 session emits GOAWAY. Matches gRPC
     * MAX_CONNECTION_AGE semantics — prevents long-lived connections
     * from pinning load on one worker behind an L4 load balancer.
     *
     * Default 0 (disabled). Recommended production value 600000
     * (10 min) for services behind L4 LB. Must be 0 or >= 1000.
     *
     * @param int $ms
     * @return static
     */
    public function setMaxConnectionAgeMs(int $ms): static {}

    /** @return int */
    public function getMaxConnectionAgeMs(): int {}

    /**
     * Set the hard-close grace after drain is signalled (milliseconds).
     *
     * If the peer hasn't closed the TCP connection this long after we
     * sent Connection: close / GOAWAY, we force-close. 0 = infinite
     * (no force-close timer armed — rely on keepalive_timeout /
     * read_timeout to clean up eventually). Non-zero values must be
     * >= 1000 to avoid sub-second timer grief.
     *
     * @param int $ms
     * @return static
     */
    public function setMaxConnectionAgeGraceMs(int $ms): static {}

    /** @return int */
    public function getMaxConnectionAgeGraceMs(): int {}

    /**
     * Set the reactive-drain spread window (milliseconds).
     *
     * When a drain event fires (CoDel trip / hard-cap transition),
     * per-connection drain effect time is uniformly distributed over
     * [0, ms] so clients don't reconnect in a thundering herd.
     * HAProxy close-spread-time analogue. Default 5000; must be
     * >= 100.
     *
     * @param int $ms
     * @return static
     */
    public function setDrainSpreadMs(int $ms): static {}

    /** @return int */
    public function getDrainSpreadMs(): int {}

    /**
     * Set the minimum gap between two reactive drain triggers
     * (milliseconds).
     *
     * Prevents drain oscillation when CoDel flips paused on/off
     * rapidly. Triggers fired during cooldown increment a telemetry
     * counter so operators can tune the value. Default 10000; must
     * be >= 1000.
     *
     * @param int $ms
     * @return static
     */
    public function setDrainCooldownMs(int $ms): static {}

    /** @return int */
    public function getDrainCooldownMs(): int {}

    // === Streaming responses (HTTP/2 Step 5b) ===

    /**
     * Per-stream chunk-queue cap for HttpResponse::send() backpressure.
     *
     * When handler's send() call grows the stream's chunk queue past
     * this many bytes, the coroutine suspends until nghttp2 drains
     * enough to drop below. HTTP/2 only; HTTP/1 chunked path uses
     * the kernel send buffer instead.
     *
     * Default: 262144 (256 KiB). Valid: 4096 .. 67108864 (64 MiB).
     * Industry: gRPC-Go 64 KiB, Envoy 1 MiB, Node.js 16 KiB.
     *
     * @param int $bytes
     * @return static
     */
    public function setStreamWriteBufferBytes(int $bytes): static {}

    /** @return int */
    public function getStreamWriteBufferBytes(): int {}

    /**
     * Set the maximum request body size accepted on both HTTP/1 and HTTP/2
     * listeners (bytes). H1 rejects with 413 + connection close; H2 rejects
     * with RST_STREAM(INTERNAL_ERROR) and the connection stays up for other
     * streams.
     *
     * Default: 10485760 (10 MiB). Valid: 1024 .. 17179869184 (16 GiB).
     *
     * @param int $bytes
     * @return static
     */
    public function setMaxBodySize(int $bytes): static {}

    /** @return int */
    public function getMaxBodySize(): int {}

    // === HTTP/3 production knobs (NEXT_STEPS.md §5) ===

    /**
     * QUIC `max_idle_timeout` (RFC 9000 §10.1) in milliseconds. Idle
     * connections close after this period of no application/ack traffic.
     *
     * Default: 30000 (30 s). RFC has no upper ceiling; valid 0 .. UINT32_MAX
     * (~49 days). 0 advertises "no idle timeout" and falls back to the
     * stack's internal default. The legacy env var
     * `PHP_HTTP3_IDLE_TIMEOUT_MS` still works as an ops escape hatch.
     *
     * @param int $ms
     * @return static
     */
    public function setHttp3IdleTimeoutMs(int $ms): static {}

    /** @return int */
    public function getHttp3IdleTimeoutMs(): int {}

    /**
     * Per-stream QUIC flow-control window. Sets all three of
     * `initial_max_stream_data_bidi_local`, `_bidi_remote`, `_uni`
     * (h2o `http3-input-window-size` style — splitting them rarely
     * helps). The connection-level `initial_max_data` is derived as
     * `window × max_concurrent_streams` (nginx pattern).
     *
     * Default: 262144 (256 KiB). Valid: 1024 .. 1073741824 (1 GiB).
     *
     * @param int $bytes
     * @return static
     */
    public function setHttp3StreamWindowBytes(int $bytes): static {}

    /** @return int */
    public function getHttp3StreamWindowBytes(): int {}

    /**
     * QUIC `initial_max_streams_bidi`. Caps how many concurrent bidi
     * streams a peer can open. Maps to nginx `http3_max_concurrent_streams`.
     *
     * Default: 100. Valid: 1 .. 1000000.
     *
     * @param int $n
     * @return static
     */
    public function setHttp3MaxConcurrentStreams(int $n): static {}

    /** @return int */
    public function getHttp3MaxConcurrentStreams(): int {}

    /**
     * Per-source-IP cap on concurrent QUIC connections. Defends against
     * handshake slow-loris and amplification by limiting fan-out from
     * a single peer. Neither h2o nor nginx exposes this directly —
     * specific to this server. Legacy env `PHP_HTTP3_PEER_BUDGET` still
     * overrides at listener spawn.
     *
     * Default: 16. Valid: 1 .. 4096.
     *
     * @param int $n
     * @return static
     */
    public function setHttp3PeerConnectionBudget(int $n): static {}

    /** @return int */
    public function getHttp3PeerConnectionBudget(): int {}

    /**
     * Toggle the RFC 7838 `Alt-Svc: h3=":<port>"; ma=86400` header
     * advertisement on H1/H2 responses when an H3 listener is up.
     * Default true. Disable during phased H3 rollout.
     *
     * Replaces the env-var-only PHP_HTTP3_DISABLE_ALT_SVC knob; the
     * env var is still honoured at start() time when present.
     *
     * @param bool $enable
     * @return static
     */
    public function setHttp3AltSvcEnabled(bool $enable): static {}

    /** @return bool */
    public function isHttp3AltSvcEnabled(): bool {}

    // === HTTP body compression (issue #8) ===

    /**
     * Master switch for HTTP body compression. When true (default), responses
     * served on H1/H2/H3 are compressed when the client advertises a
     * supported encoding via Accept-Encoding and the response satisfies
     * the policy filters (size, MIME, no Range, etc.).
     *
     * Default: true. When the extension is built without
     * --enable-http-compression, only setCompressionEnabled(false) is
     * accepted — passing true throws.
     *
     * @param bool $enable
     * @return static
     */
    public function setCompressionEnabled(bool $enable): static {}

    /** @return bool */
    public function isCompressionEnabled(): bool {}

    /**
     * Compression level. zlib semantics: 1 = fastest/weakest,
     * 9 = slowest/strongest, 6 = balanced default.
     *
     * Default: 6. Valid: 1..9.
     *
     * @param int $level
     * @return static
     */
    public function setCompressionLevel(int $level): static {}

    /** @return int */
    public function getCompressionLevel(): int {}

    /**
     * Brotli quality (issue #9). Default: 4 (production-typical;
     * quality 11 is research-quality, roughly 50× slower than 4 with
     * marginal extra ratio). Range: 0..11. Throws on out-of-range or
     * if the config is locked.
     *
     * Inert when the extension was built without --enable-brotli — the
     * response pipeline never selects Brotli without HAVE_HTTP_BROTLI,
     * regardless of what this setter is called with.
     *
     * @param int $level
     * @return static
     */
    public function setBrotliLevel(int $level): static {}

    /** @return int */
    public function getBrotliLevel(): int {}

    /**
     * zstd compression level (issue #9). Default: 3 (the zstd team's
     * own production default — better ratio than gzip-6 at higher
     * throughput). Range: 1..22.
     *
     * @param int $level
     * @return static
     */
    public function setZstdLevel(int $level): static {}

    /** @return int */
    public function getZstdLevel(): int {}

    /**
     * Default JSON_* flags applied by HttpResponse::json() when the
     * per-call $flags argument is 0 (or omitted). Bitmask of PHP's
     * `JSON_*` constants — same values as `json_encode()`.
     *
     * Default: `JSON_UNESCAPED_UNICODE | JSON_UNESCAPED_SLASHES`
     * (smaller wire size for non-ASCII text + readable URLs in
     * payloads).
     *
     * `JSON_THROW_ON_ERROR` is silently stripped at call time — encode
     * failure yields a 500 JSON error body, not a propagated exception.
     *
     * @param int $flags
     * @return static
     */
    public function setJsonEncodeFlags(int $flags): static {}

    /** @return int */
    public function getJsonEncodeFlags(): int {}

    /**
     * Codecs compiled into this build, in server preference order.
     * Always contains "identity"; "gzip" present iff
     * --enable-http-compression succeeded; "br" / "zstd" present iff
     * the corresponding library was found at configure time.
     *
     * @return string[]
     */
    public static function getSupportedEncodings(): array {}

    /**
     * Body-size threshold below which responses are left uncompressed
     * (the encoding overhead beats any real-world win on tiny bodies).
     *
     * Default: 1024 (1 KiB). Valid: 0..16 MiB.
     *
     * @param int $bytes
     * @return static
     */
    public function setCompressionMinSize(int $bytes): static {}

    /** @return int */
    public function getCompressionMinSize(): int {}

    /**
     * MIME-type whitelist eligible for compression. REPLACES the current
     * list wholesale (nginx `gzip_types` semantics). Entries are
     * normalised at setter time: parameters (`; charset=…`) stripped,
     * whitespace trimmed, lowercased — so the per-request match is
     * exact and zero-allocation.
     *
     * Default: ["application/javascript", "application/json",
     * "application/xml", "image/svg+xml", "text/css", "text/html",
     * "text/javascript", "text/plain", "text/xml"].
     *
     * @param string[] $types
     * @return static
     */
    public function setCompressionMimeTypes(array $types): static {}

    /** @return string[] The materialised whitelist */
    public function getCompressionMimeTypes(): array {}

    /**
     * Anti-zip-bomb cap on decoded request bodies (Content-Encoding: gzip
     * inbound). Decoders abort and the request fails with 413 once the
     * decompressed byte count exceeds this. 0 disables the cap entirely
     * (must be set explicitly — there is no implicit "unlimited" path).
     *
     * Default: 10485760 (10 MiB).
     *
     * @param int $bytes
     * @return static
     */
    public function setRequestMaxDecompressedSize(int $bytes): static {}

    /** @return int */
    public function getRequestMaxDecompressedSize(): int {}

    // === Buffers ===

    /**
     * Set write buffer size
     *
     * @param int $size Buffer size in bytes
     * @return static
     */
    public function setWriteBufferSize(int $size): static {}

    /**
     * Get write buffer size
     */
    public function getWriteBufferSize(): int {}

    // === Protocol options ===

    /**
     * Enable HTTP/2 support (TODO)
     *
     * @param bool $enable Enable HTTP/2
     * @return static
     */
    public function enableHttp2(bool $enable): static {}

    /**
     * Check if HTTP/2 is enabled
     */
    public function isHttp2Enabled(): bool {}

    /**
     * Enable WebSocket support (TODO)
     *
     * @param bool $enable Enable WebSocket
     * @return static
     */
    public function enableWebSocket(bool $enable): static {}

    /**
     * Check if WebSocket is enabled
     */
    public function isWebSocketEnabled(): bool {}

    /**
     * Enable automatic protocol detection
     *
     * @param bool $enable Enable detection
     * @return static
     */
    public function enableProtocolDetection(bool $enable): static {}

    /**
     * Check if protocol detection is enabled
     */
    public function isProtocolDetectionEnabled(): bool {}

    // === TLS configuration (TODO) ===

    /**
     * Enable TLS for default listener
     *
     * @param bool $enable Enable TLS
     * @return static
     */
    public function enableTls(bool $enable): static {}

    /**
     * Check if TLS is enabled
     */
    public function isTlsEnabled(): bool {}

    /**
     * Set TLS certificate file
     *
     * @param string $path Path to certificate file (PEM)
     * @return static
     */
    public function setCertificate(string $path): static {}

    /**
     * Get certificate path
     */
    public function getCertificate(): ?string {}

    /**
     * Set TLS private key file
     *
     * @param string $path Path to private key file (PEM)
     * @return static
     */
    public function setPrivateKey(string $path): static {}

    /**
     * Get private key path
     */
    public function getPrivateKey(): ?string {}

    // === Body handling ===

    /**
     * Set auto-await mode for request body
     *
     * When enabled, non-multipart requests wait for full body before handler is called.
     * Multipart requests always use streaming.
     *
     * @param bool $enable Enable auto-await
     * @return static
     */
    public function setAutoAwaitBody(bool $enable): static {}

    /**
     * Check if auto-await is enabled
     */
    public function isAutoAwaitBodyEnabled(): bool {}

    // === Logging / telemetry ===

    /**
     * Set minimum log severity. The logger is disabled by default
     * (LogSeverity::OFF). Setting any non-OFF value plus a stream via
     * setLogStream() activates the logger at server start.
     *
     * Severity is fixed at server start — runtime changes are not
     * supported (single-threaded, lock-free model).
     *
     * @return static
     */
    public function setLogSeverity(\TrueAsync\LogSeverity $level): static {}

    /**
     * Get currently configured log severity.
     */
    public function getLogSeverity(): \TrueAsync\LogSeverity {}

    /**
     * Set log sink. Accepts any php_stream resource (file, php://stderr,
     * php://memory, user wrapper). Logger remains disabled until both
     * a non-OFF severity and a stream are supplied.
     *
     * @param resource|null $stream A php_stream resource, or null to clear.
     * @return static
     */
    public function setLogStream(mixed $stream): static {}

    /**
     * Get configured log sink stream, or null if unset.
     *
     * @return resource|null
     */
    public function getLogStream(): mixed {}

    /**
     * Enable or disable telemetry. When enabled, the server parses
     * incoming traceparent / tracestate headers (W3C Trace Context) and
     * attaches them to the request — accessible via HttpRequest API.
     *
     * @return static
     */
    public function setTelemetryEnabled(bool $enabled): static {}

    /**
     * Check whether telemetry is enabled.
     */
    public function isTelemetryEnabled(): bool {}

    // === State ===

    /**
     * Check if config is locked (after server start)
     *
     * Locked config cannot be modified.
     */
    public function isLocked(): bool {}
}
