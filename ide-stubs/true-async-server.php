<?php

/**
 * TrueAsync HTTP Server extension stubs for IDEs.
 *
 * Provides IDE-level type information for the `true_async_server` PHP
 * extension — a coroutine-native HTTP/1.1, HTTP/2 and HTTP/3 server
 * built on the TrueAsync runtime.
 *
 * This file is never loaded at runtime; it exists purely so editors
 * (PhpStorm, VS Code Intelephense, …) can resolve the classes, enums
 * and functions the extension registers.
 *
 * @since      8.6
 * @version    0.6.5
 * @link       https://github.com/true-async/server
 */

declare(strict_types=1);

namespace TrueAsync {

// ---------------------------------------------------------------------------
// Exceptions
// ---------------------------------------------------------------------------

/**
 * Base exception for all HTTP server errors.
 */
class HttpServerException extends \Exception
{
}

/**
 * Runtime errors during server operation.
 */
final class HttpServerRuntimeException extends HttpServerException
{
}

/**
 * Invalid argument passed to server methods.
 */
final class HttpServerInvalidArgumentException extends HttpServerException
{
}

/**
 * Connection-related errors (socket, network).
 */
final class HttpServerConnectionException extends HttpServerException
{
}

/**
 * Protocol-level errors (malformed HTTP, invalid headers).
 */
final class HttpServerProtocolException extends HttpServerException
{
}

/**
 * Timeout errors (read, write, keep-alive).
 */
final class HttpServerTimeoutException extends HttpServerException
{
}

/**
 * Throw from a handler to send a specific HTTP error response. The server
 * reads $code as the HTTP status (must be in 4xx/5xx, otherwise 500 is used)
 * and $message as the response body.
 *
 * Also raised internally when the parser hits a limit AFTER the handler
 * coroutine was dispatched: we cancel the handler with HttpException so the
 * cancellation propagates through the normal Async cancellation chain
 * (extends AsyncCancellation) while carrying the precise HTTP status to
 * send back to the peer.
 *
 * NOT marked final — user code may extend it for richer typing
 * (NotFoundException extends HttpException etc).
 */
class HttpException extends \Async\AsyncCancellation
{
}

// ---------------------------------------------------------------------------
// Enums
// ---------------------------------------------------------------------------

/**
 * Logger severity levels.
 *
 * Backing values match the OpenTelemetry Logs Data Model SeverityNumber
 * (1-24). Only OFF + a stable subset of OTel buckets are exposed here:
 *
 *   - DEBUG (5)  — verbose, diagnostic-only output (h3 packet trace, etc.)
 *   - INFO  (9)  — server lifecycle (start/stop), bind retries
 *   - WARN  (13) — TLS handshake fail, peer reset, absorbed exceptions
 *   - ERROR (17) — listener bind failed, hard protocol error
 *
 * TRACE / FATAL are intentionally absent — TRACE is unused, FATAL is
 * delivered via zend_error_noreturn(E_ERROR) which already terminates.
 */
enum LogSeverity: int
{
    case OFF   = 0;
    case DEBUG = 5;
    case INFO  = 9;
    case WARN  = 13;
    case ERROR = 17;
}

/**
 * Content-Disposition for HttpResponse::sendFile().
 */
enum SendFileDisposition: string
{
    case INLINE     = 'inline';
    case ATTACHMENT = 'attachment';
}

/**
 * Behavior when a requested path does not resolve to a regular file
 * inside the static handler's root directory.
 *
 * NOT_FOUND (default): the static handler emits a 404 in C and the
 *                      request never enters the PHP VM.
 * NEXT:                the static handler returns control to the
 *                      dispatcher, which spawns the regular handler
 *                      coroutine — the request is delivered to
 *                      {@see HttpServer::addHttpHandler()}.
 */
enum StaticOnMissing: int
{
    case NOT_FOUND = 0;
    case NEXT      = 1;
}

/**
 * Dotfile policy. A "dotfile" is any path segment that begins with
 * a literal `.` — including `..` (which is also rejected by the
 * traversal guard regardless of this policy).
 *
 * DENY   (default): 404 on any request whose resolved path traverses
 *                   a dotfile component.
 * ALLOW:            dotfiles are served like any other file.
 * IGNORE:           treat as if the file does not exist (passthrough
 *                   per {@see StaticOnMissing}).
 */
enum StaticDotfiles: int
{
    case DENY   = 0;
    case ALLOW  = 1;
    case IGNORE = 2;
}

/**
 * Symlink policy applied during path resolution.
 *
 * REJECT      (default): symlinks anywhere on the resolved path yield
 *                        404. Implemented via O_NOFOLLOW + per-segment
 *                        lstat — no symlink is ever traversed.
 * FOLLOW:                symlinks are followed normally; the post-
 *                        realpath() target must still live inside the
 *                        configured root directory.
 * OWNER_MATCH:           follow only if the symlink and its final
 *                        target are owned by the same uid.
 */
enum StaticSymlinks: int
{
    case REJECT      = 0;
    case FOLLOW      = 1;
    case OWNER_MATCH = 2;
}

// ---------------------------------------------------------------------------
// Value objects
// ---------------------------------------------------------------------------

/**
 * Options for HttpResponse::sendFile(). Value object, immutable.
 */
final readonly class SendFileOptions
{
    public function __construct(
        public ?string             $contentType     = null,
        public SendFileDisposition $disposition     = SendFileDisposition::INLINE,
        public ?string             $downloadName    = null,
        public ?string             $cacheControl    = null,
        public bool                $etag            = true,
        public bool                $lastModified    = true,
        public bool                $acceptRanges    = true,
        public bool                $precompressed   = true,
        public bool                $conditional     = true,
        public bool                $deleteAfterSend = false,
        public ?int                $status          = null,
    ) {}
}

/**
 * Represents an uploaded file (PSR-7 compatible).
 */
final class UploadedFile
{
    /**
     * Get stream resource for reading the file.
     * Can read partially uploaded file.
     *
     * @return resource|null Stream resource or null if not available
     * @throws \RuntimeException if file has already been moved
     */
    public function getStream(): mixed {}

    /**
     * Move the uploaded file to a new location.
     * - Supports relative and absolute paths
     * - Automatically creates directory if it doesn't exist
     * - Cross-filesystem: automatic fallback to copy()+unlink()
     *
     * @param string $targetPath Target file path
     * @param int $mode File permissions (default 0644)
     * @throws \RuntimeException if file has already been moved
     * @throws \RuntimeException on write error
     */
    public function moveTo(string $targetPath, int $mode = 0644): void {}

    /**
     * Get file size in bytes.
     */
    public function getSize(): ?int {}

    /**
     * Get upload error code (UPLOAD_ERR_* constants).
     */
    public function getError(): int {}

    /**
     * Get original filename from client (as-is, no modifications).
     * Limit: 4KB.
     */
    public function getClientFilename(): ?string {}

    /**
     * Get MIME type from client (trusted from browser).
     */
    public function getClientMediaType(): ?string {}

    /**
     * Get charset from Content-Type header (if specified).
     */
    public function getClientCharset(): ?string {}

    /**
     * Check if file is fully uploaded and ready (after fclose).
     */
    public function isReady(): bool {}

    /**
     * Check if file was successfully uploaded (getError() === UPLOAD_ERR_OK).
     */
    public function isValid(): bool {}
}

// ---------------------------------------------------------------------------
// Static file handler
// ---------------------------------------------------------------------------

/**
 * Built-in static file handler (issue #13).
 *
 * Configures one prefix-rooted static mount; attach to a server with
 * {@see HttpServer::addStaticHandler()}. Multiple mounts are allowed
 * and matched in registration order.
 *
 * The handler runs entirely in C without spawning a coroutine — files
 * are served via libuv async fs ops directly into the response stream.
 * No PHP callbacks fire on the static path.
 *
 * Note: a request whose URL maps to a directory and whose configured
 * index files all 404 returns 404 (or falls through per StaticOnMissing
 * for `Next`). This handler does NOT issue the 301 redirect that nginx
 * / Apache emit when a directory URL is missing the trailing slash;
 * call `setIndexFiles([])` / `disableIndex()` if your deployment relies
 * on a real catch-all on directory paths.
 */
final class StaticHandler
{
    /**
     * @param string $urlPrefix    URL path prefix (e.g. "/static/").
     *                             Must start with `/` and end with `/`.
     * @param string $rootDirectory Filesystem directory whose contents
     *                             are exposed under $urlPrefix. Must be
     *                             an absolute path; canonicalised at
     *                             attach time.
     */
    public function __construct(string $urlPrefix, string $rootDirectory) {}

    // === Index / fallthrough ===

    /**
     * Files served when the request resolves to a directory. Defaults
     * to ["index.html"]. Pass an empty list to disable index lookup.
     *
     * @return static
     */
    public function setIndexFiles(string ...$files): static {}

    /**
     * Disable directory-index lookup. Equivalent to setIndexFiles().
     *
     * @return static
     */
    public function disableIndex(): static {}

    /**
     * Behaviour when no file matches the request.
     *
     * @return static
     */
    public function setOnMissing(StaticOnMissing $mode): static {}

    // === Precompressed sidecars ===

    /**
     * Enable serving precompressed sidecar files (e.g. `main.css.br`,
     * `main.css.gz`) when the client's Accept-Encoding header allows.
     *
     * Each argument is a content-coding name: "br", "gzip", or "zstd".
     * Throws InvalidArgumentException at the setter for unknown names.
     *
     * @return static
     */
    public function enablePrecompressed(string ...$encodings): static {}

    /**
     * Disable precompressed sidecar lookup.
     *
     * @return static
     */
    public function disablePrecompressed(): static {}

    // === Security ===

    /** @return static */
    public function setDotfilePolicy(StaticDotfiles $policy): static {}

    /** @return static */
    public function setSymlinkPolicy(StaticSymlinks $policy): static {}

    /**
     * Glob patterns whose matching paths return 404 regardless of
     * existence. Patterns are matched against the path *relative to
     * the root directory*, with `/` as the separator.
     *
     * @return static
     */
    public function hide(string ...$globs): static {}

    // === Cache / headers ===

    /**
     * Toggle weak ETag emission (default true). When enabled, every
     * 200 response carries an `ETag: W/"…"` header derived from
     * `(mtime_ns, size, ino)`; If-None-Match / If-Modified-Since
     * yield 304.
     *
     * @return static
     */
    public function setEtagEnabled(bool $enabled): static {}

    /**
     * Set the literal `Cache-Control` value. Pass an empty string to
     * suppress emission.
     *
     * @return static
     */
    public function setCacheControl(string $value): static {}

    /**
     * Enable the nginx-style open-file cache for this mount. The cache
     * stores the resolved path, fstat metadata, MIME content-type, ETag
     * and Last-Modified bytes for the most recent $maxEntries requests;
     * within $ttlSeconds, repeated requests hit the cache and skip the
     * realpath/stat/MIME-lookup walk.
     *
     * Off by default. The cache earns its keep on cold-dentry / large-
     * docroot / network-FS workloads; on warm-dentry local serving
     * the syscalls being skipped are already only a few microseconds
     * each so the HashTable lookup overhead is net-negative.
     *
     * Pass $maxEntries == 0 to disable.
     *
     * @return static
     */
    public function setOpenFileCache(int $maxEntries, int $ttlSeconds = 60): static {}

    /**
     * Sugar for setOpenFileCache(0).
     *
     * @return static
     */
    public function disableOpenFileCache(): static {}

    /**
     * Add a fixed response header. Evaluated once at attach time and
     * emitted on every 200 response (and on 304 except for
     * Content-* headers per RFC 9110 §15.4.5).
     *
     * @return static
     */
    public function setHeader(string $name, string $value): static {}

    // === Directory listing ===

    /**
     * Toggle directory-listing HTML when the request resolves to a
     * directory and no index file matches. Default false.
     *
     * Reserved for PR #6 — currently a no-op accepted at the setter.
     *
     * @return static
     */
    public function setBrowseEnabled(bool $enabled): static {}

    // === MIME ===

    /**
     * Override the Content-Type for files with the given extension.
     * Extension is lowercased; do not include the leading dot.
     *
     * @return static
     */
    public function setMimeType(string $extension, string $contentType): static {}

    // === Introspection ===

    /** @return string */
    public function getUrlPrefix(): string {}

    /** @return string */
    public function getRootDirectory(): string {}

    /**
     * True once attached to an HttpServer via addStaticHandler().
     * Locked handlers reject all setters with a runtime exception.
     */
    public function isLocked(): bool {}
}

// ---------------------------------------------------------------------------
// Server configuration
// ---------------------------------------------------------------------------

/**
 * HTTP Server configuration.
 */
final class HttpServerConfig
{
    /**
     * Create server configuration.
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
     * Add HTTP/3 (QUIC over UDP) listener.
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
     * Get all configured listeners.
     *
     * @return array Array of listener configurations
     */
    public function getListeners(): array {}

    // === Connection limits ===

    /**
     * Set socket backlog (pending connections queue).
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
     * Optional bootloader Closure handed to the built-in worker pool.
     *
     * The pool deep-copies the closure once and runs it on every worker
     * before that worker's task loop — the right place for per-worker
     * autoload, DB pool warm-up, opcache primes, or any other one-shot
     * init that would otherwise need to run inside the handler closure
     * on every request.
     *
     * Only consulted when {@see setWorkers()} > 1. Pass `null` to clear.
     *
     * @return static
     */
    public function setBootloader(?\Closure $bootloader): static {}

    /**
     * Returns the bootloader previously set, or null.
     */
    public function getBootloader(): ?\Closure {}

    /**
     * Get socket backlog.
     */
    public function getBacklog(): int {}

    /**
     * Set maximum concurrent connections.
     *
     * @param int $maxConnections Max connections (0 = unlimited)
     * @return static
     */
    public function setMaxConnections(int $maxConnections): static {}

    /**
     * Get maximum connections.
     */
    public function getMaxConnections(): int {}

    /**
     * Set maximum concurrent in-flight requests (overload shedding).
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
     * Set read timeout (for receiving request).
     *
     * @param int $timeout Timeout in seconds (0 = no timeout)
     * @return static
     */
    public function setReadTimeout(int $timeout): static {}

    /**
     * Get read timeout.
     */
    public function getReadTimeout(): int {}

    /**
     * Set write timeout (for sending response).
     *
     * @param int $timeout Timeout in seconds (0 = no timeout)
     * @return static
     */
    public function setWriteTimeout(int $timeout): static {}

    /**
     * Get write timeout.
     */
    public function getWriteTimeout(): int {}

    /**
     * Set keep-alive timeout (idle time between requests).
     *
     * @param int $timeout Timeout in seconds (0 = disable keep-alive)
     * @return static
     */
    public function setKeepAliveTimeout(int $timeout): static {}

    /**
     * Get keep-alive timeout.
     */
    public function getKeepAliveTimeout(): int {}

    /**
     * Set shutdown timeout (graceful shutdown).
     *
     * @param int $timeout Timeout in seconds
     * @return static
     */
    public function setShutdownTimeout(int $timeout): static {}

    /**
     * Get shutdown timeout.
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

    // === Connection draining ===

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

    // === Streaming responses ===

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
     * Set the per-worker memory cap for HTTP/2 static-file body buffers
     * (read-ahead chunks + ring queues). 0 = auto (memory_limit/8). Any
     * explicit value is clamped so the static budget never exceeds
     * memory_limit minus a small reserve for PHP heap + nghttp2/TLS
     * overhead.
     *
     * @param int $bytes  0 for auto, otherwise byte ceiling.
     * @return static
     */
    public function setH2StaticBudgetMax(int $bytes): static {}

    /** @return int  0 if not explicitly set (auto = memory_limit/8) */
    public function getH2StaticBudgetMax(): int {}

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

    // === HTTP/3 production knobs ===

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

    // === HTTP body compression ===

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
     * Set write buffer size.
     *
     * @param int $size Buffer size in bytes
     * @return static
     */
    public function setWriteBufferSize(int $size): static {}

    /**
     * Get write buffer size.
     */
    public function getWriteBufferSize(): int {}

    // === Protocol options ===

    /**
     * Enable HTTP/2 support.
     *
     * @param bool $enable Enable HTTP/2
     * @return static
     */
    public function enableHttp2(bool $enable): static {}

    /**
     * Check if HTTP/2 is enabled.
     */
    public function isHttp2Enabled(): bool {}

    /**
     * Enable WebSocket support.
     *
     * @param bool $enable Enable WebSocket
     * @return static
     */
    public function enableWebSocket(bool $enable): static {}

    /**
     * Check if WebSocket is enabled.
     */
    public function isWebSocketEnabled(): bool {}

    /**
     * Enable automatic protocol detection.
     *
     * @param bool $enable Enable detection
     * @return static
     */
    public function enableProtocolDetection(bool $enable): static {}

    /**
     * Check if protocol detection is enabled.
     */
    public function isProtocolDetectionEnabled(): bool {}

    // === TLS configuration ===

    /**
     * Enable TLS for default listener.
     *
     * @param bool $enable Enable TLS
     * @return static
     */
    public function enableTls(bool $enable): static {}

    /**
     * Check if TLS is enabled.
     */
    public function isTlsEnabled(): bool {}

    /**
     * Set TLS certificate file.
     *
     * @param string $path Path to certificate file (PEM)
     * @return static
     */
    public function setCertificate(string $path): static {}

    /**
     * Get certificate path.
     */
    public function getCertificate(): ?string {}

    /**
     * Set TLS private key file.
     *
     * @param string $path Path to private key file (PEM)
     * @return static
     */
    public function setPrivateKey(string $path): static {}

    /**
     * Get private key path.
     */
    public function getPrivateKey(): ?string {}

    // === Body handling ===

    /**
     * Set auto-await mode for request body.
     *
     * When enabled, non-multipart requests wait for full body before handler is called.
     * Multipart requests always use streaming.
     *
     * @param bool $enable Enable auto-await
     * @return static
     */
    public function setAutoAwaitBody(bool $enable): static {}

    /**
     * Check if auto-await is enabled.
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
    public function setLogSeverity(LogSeverity $level): static {}

    /**
     * Get currently configured log severity.
     */
    public function getLogSeverity(): LogSeverity {}

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

    // === Request-body streaming ===

    /**
     * Stream request bodies into a per-request queue (issue #26) instead
     * of accumulating into `req->body`. Handlers must consume via
     * {@see HttpRequest::awaitBody()}; getBody() throws.
     *
     * @return static
     */
    public function setBodyStreamingEnabled(bool $enabled): static {}

    /**
     * Check whether request-body streaming is enabled.
     */
    public function isBodyStreamingEnabled(): bool {}

    // === State ===

    /**
     * Check if config is locked (after server start).
     *
     * Locked config cannot be modified.
     */
    public function isLocked(): bool {}
}

// ---------------------------------------------------------------------------
// Server
// ---------------------------------------------------------------------------

/**
 * HTTP Server.
 */
final class HttpServer
{
    /**
     * Create HTTP server with configuration.
     *
     * @param HttpServerConfig $config Server configuration
     */
    public function __construct(HttpServerConfig $config) {}

    /**
     * Add HTTP/1.1 request handler.
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
     * Add WebSocket handler.
     *
     * @param callable $handler WebSocket handler callback
     * @return static
     */
    public function addWebSocketHandler(callable $handler): static {}

    /**
     * Add HTTP/2 handler.
     *
     * @param callable $handler HTTP/2 handler callback
     * @return static
     */
    public function addHttp2Handler(callable $handler): static {}

    /**
     * Add gRPC handler.
     *
     * @param callable $handler gRPC handler callback
     * @return static
     */
    public function addGrpcHandler(callable $handler): static {}

    /**
     * Start server and begin accepting connections.
     *
     * This method blocks until stop() is called or an error occurs.
     *
     * @return bool True if started successfully
     */
    public function start(): bool {}

    /**
     * Stop server gracefully.
     *
     * Stops accepting new connections, waits for active requests to complete
     * (up to shutdown timeout), then closes all connections.
     *
     * @return bool True if stopped successfully
     */
    public function stop(): bool {}

    /**
     * Check if server is running.
     */
    public function isRunning(): bool {}

    /**
     * Get server telemetry.
     *
     * @return array Telemetry data
     */
    public function getTelemetry(): array {}

    /**
     * Reset telemetry counters.
     *
     * @return bool True if reset successfully
     */
    public function resetTelemetry(): bool {}

    /**
     * Get server configuration.
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

// ---------------------------------------------------------------------------
// Request
// ---------------------------------------------------------------------------

/**
 * HTTP Request representation (read-only).
 *
 * Instances are created internally by the server and passed to the
 * registered handler — never constructed from user code.
 */
final class HttpRequest
{
    /**
     * Private constructor — instances created internally by server.
     */
    private function __construct() {}

    /**
     * Get HTTP method (GET, POST, PUT, DELETE, etc.).
     */
    public function getMethod(): string {}

    /**
     * Get request URI (path + query string).
     */
    public function getUri(): string {}

    /**
     * Get path component of the URI (no query string).
     */
    public function getPath(): string {}

    /**
     * Get all query parameters as an associative array.
     * Supports PHP array notation: foo[bar], foo[]
     *
     * @return array<string, mixed>
     */
    public function getQuery(): array {}

    /**
     * Get a single query parameter by name.
     *
     * @param string $name  Parameter name
     * @param mixed  $default  Value to return when the parameter is absent (default: null)
     * @return mixed
     */
    public function getQueryParam(string $name, mixed $default = null): mixed {}

    /**
     * Get HTTP version string (e.g., "1.1").
     */
    public function getHttpVersion(): string {}

    /**
     * Check if header exists (case-insensitive).
     */
    public function hasHeader(string $name): bool {}

    /**
     * Get single header value by name (case-insensitive).
     * Returns null if header doesn't exist.
     */
    public function getHeader(string $name): ?string {}

    /**
     * Get header line (all values comma-separated).
     * Returns empty string if header doesn't exist.
     */
    public function getHeaderLine(string $name): string {}

    /**
     * Get all headers as associative array.
     * Header names are lowercase.
     *
     * @return array<string, string>
     */
    public function getHeaders(): array {}

    /**
     * Get request body.
     * Returns empty string if no body.
     */
    public function getBody(): string {}

    /**
     * Check if request has a body.
     */
    public function hasBody(): bool {}

    /**
     * Check if connection should be kept alive.
     */
    public function isKeepAlive(): bool {}

    /**
     * Get POST data from multipart/form-data or application/x-www-form-urlencoded.
     * Supports PHP-style arrays: name[], user[name], matrix[0][1]
     *
     * @return array
     */
    public function getPost(): array {}

    /**
     * Get all uploaded files.
     * Multiple files with same name: ['photos' => [UploadedFile, UploadedFile, ...]]
     *
     * @return array
     */
    public function getFiles(): array {}

    /**
     * Get single uploaded file by name.
     * For multiple files (photos[]), returns first one.
     *
     * @param string $name Field name
     * @return UploadedFile|null File object or null if not found
     */
    public function getFile(string $name): ?UploadedFile {}

    /**
     * Get Content-Type header value.
     * Returns null if not set.
     */
    public function getContentType(): ?string {}

    /**
     * Get Content-Length header value.
     * Returns null if not set or invalid.
     */
    public function getContentLength(): ?int {}

    /**
     * W3C Trace Context — raw `traceparent` header as received, or null
     * if the header was missing / malformed / telemetry is disabled.
     */
    public function getTraceParent(): ?string {}

    /**
     * W3C Trace Context — raw `tracestate` header as received, or null
     * if absent / telemetry is disabled.
     */
    public function getTraceState(): ?string {}

    /**
     * Decoded 32-character lower-hex trace_id, or null if no valid
     * traceparent was ingested.
     */
    public function getTraceId(): ?string {}

    /**
     * Decoded 16-character lower-hex parent span_id, or null if no
     * valid traceparent was ingested.
     */
    public function getSpanId(): ?string {}

    /**
     * Decoded 8-bit trace flags byte (e.g. 0x01 = sampled), or null
     * if no valid traceparent was ingested.
     */
    public function getTraceFlags(): ?int {}

    /**
     * Wait for the complete request body.
     *
     * Once streaming dispatch is enabled (Phase 6 Step 3+), the handler
     * is called as soon as headers are parsed — before the body has been
     * received. Calling awaitBody() suspends the current coroutine until
     * the body_event on this request fires message-complete.
     *
     * When the body is already fully buffered (the current default), this
     * call returns immediately without suspending.
     *
     * @return static
     */
    public function awaitBody(): static {}

    /**
     * Read the next chunk of a streamed request body (issue #26).
     *
     * Used when request-body streaming is enabled via
     * {@see HttpServerConfig::setBodyStreamingEnabled()}: the handler is
     * invoked as soon as headers are parsed and pulls the body
     * incrementally instead of having it buffered into getBody().
     *
     * Suspends the current coroutine until at least one byte is
     * available, then returns up to $maxLen bytes. Returns null once the
     * body has been fully consumed (end of stream).
     *
     * @param int $maxLen Maximum number of bytes to return (default 65536).
     * @return string|null Next chunk, or null at end of stream.
     */
    public function readBody(int $maxLen = 65536): ?string {}
}

// ---------------------------------------------------------------------------
// Response
// ---------------------------------------------------------------------------

/**
 * HTTP Response (fluent interface).
 *
 * Instances are created internally by the server and passed to the
 * registered handler — never constructed from user code.
 */
final class HttpResponse
{
    /**
     * Private constructor — instances created internally by server.
     */
    private function __construct() {}

    // === Status methods ===

    /**
     * Set response status code.
     *
     * @param int $code HTTP status code (100-599)
     * @return static
     */
    public function setStatusCode(int $code): static {}

    /**
     * Get response status code.
     */
    public function getStatusCode(): int {}

    /**
     * Set response reason phrase.
     *
     * @param string $phrase Reason phrase (e.g., "OK", "Not Found")
     * @return static
     */
    public function setReasonPhrase(string $phrase): static {}

    /**
     * Get response reason phrase.
     */
    public function getReasonPhrase(): string {}

    // === Header methods ===

    /**
     * Set header (replaces existing).
     *
     * @param string $name Header name
     * @param string|array $value Header value(s)
     * @return static
     */
    public function setHeader(string $name, string|array $value): static {}

    /**
     * Add header value (appends to existing).
     *
     * @param string $name Header name
     * @param string|array $value Header value(s)
     * @return static
     */
    public function addHeader(string $name, string|array $value): static {}

    /**
     * Check if header exists.
     *
     * @param string $name Header name (case-insensitive)
     */
    public function hasHeader(string $name): bool {}

    /**
     * Get header value (first value if multiple).
     *
     * @param string $name Header name (case-insensitive)
     * @return string|null Header value or null if not exists
     */
    public function getHeader(string $name): ?string {}

    /**
     * Get header line (all values comma-separated).
     *
     * @param string $name Header name (case-insensitive)
     */
    public function getHeaderLine(string $name): string {}

    /**
     * Get all headers.
     *
     * @return array Headers with all values
     */
    public function getHeaders(): array {}

    /**
     * Reset all headers.
     *
     * @return static
     */
    public function resetHeaders(): static {}

    // === Trailer methods (HTTP/2 only) ===

    /**
     * Set an HTTP/2 response trailer — delivered after the body as a
     * terminal HEADERS frame. The canonical consumer is gRPC, which
     * carries its status code in a `grpc-status` trailer. On HTTP/1
     * the value is silently dropped (no chunked-encoding trailer
     * emission in Step 5b's scope).
     *
     * @param string $name  Lowercase header name (RFC 9113 §8.2.2;
     *                      uppercase values get lowercased on wire).
     * @param string $value Header value.
     * @return static
     */
    public function setTrailer(string $name, string $value): static {}

    /**
     * Bulk-set trailers from an associative array of name => value.
     * Equivalent to calling setTrailer() in a loop. Existing trailers
     * are preserved — use resetTrailers() first for a clean slate.
     */
    public function setTrailers(array $trailers): static {}

    /**
     * Remove every previously-set trailer. Safe to call even if
     * none were set.
     */
    public function resetTrailers(): static {}

    /**
     * Get all trailers as a name => value array. Returns an empty
     * array when none were set.
     */
    public function getTrailers(): array {}

    // === Protocol methods ===

    /**
     * Get protocol name (always "HTTP").
     */
    public function getProtocolName(): string {}

    /**
     * Get protocol version (e.g., "1.1", "2").
     */
    public function getProtocolVersion(): string {}

    // === Body methods ===

    /**
     * Write data to response body buffer.
     *
     * @param string $data Data to write
     * @return static
     */
    public function write(string $data): static {}

    /**
     * Send a chunk to the client (streaming response).
     *
     * First call commits status + headers (they can no longer be
     * changed). Subsequent calls append DATA frames (HTTP/2) or
     * chunked-transfer segments (HTTP/1).
     *
     * Blocks the handler coroutine ONLY under backpressure — when the
     * per-stream staging buffer is full (HTTP/2: all ring slots live
     * OR queued bytes reach HttpServerConfig::setStreamWriteBufferBytes,
     * default 256 KiB). Otherwise returns immediately. send() is always
     * safe to call; use sendable() to check first if you'd rather do
     * other work than block.
     *
     * @param string $chunk
     * @return static
     */
    public function send(string $chunk): static {}

    /**
     * Advisory, non-blocking backpressure check for streaming responses.
     *
     * Returns true when send() would accept a chunk without suspending
     * the handler coroutine — the per-stream staging buffer has room.
     * Returns false when send() would block on backpressure, or when the
     * response is closed / sealed by sendFile() / not streaming-capable.
     *
     * send() is always safe to call regardless; sendable() just lets a
     * handler do other work instead of blocking on a slow peer.
     *
     * @return bool
     */
    public function sendable(): bool {}

    /**
     * Mark this response as ineligible for compression. Overrides every
     * other rule (Accept-Encoding negotiation, MIME whitelist, size
     * threshold). Use for endpoints that combine secrets with reflected
     * user input (BREACH mitigation), responses already bearing a
     * Content-Encoding the handler set itself, or any payload the
     * server must not wrap. Idempotent.
     *
     * @return static
     */
    public function setNoCompression(): static {}

    /**
     * Get current body content.
     */
    public function getBody(): string {}

    /**
     * Set body content (replaces buffer).
     *
     * @param string $body Body content
     * @return static
     */
    public function setBody(string $body): static {}

    /**
     * Get body stream.
     *
     * @return mixed Stream resource or null
     */
    public function getBodyStream(): mixed {}

    /**
     * Set body stream.
     *
     * @param mixed $stream Stream resource
     * @return static
     */
    public function setBodyStream(mixed $stream): static {}

    // === Helper methods ===

    /**
     * Set the response body to a JSON payload.
     *
     *  - `array` / `object` / scalar `$data` → encoded via the same
     *    `php_json_encode_ex` that powers `json_encode()`.
     *  - `string` `$data` → shipped as-is. Use this when you already
     *    have JSON bytes (cached, pre-built, fetched from another
     *    service) — skips re-encoding entirely.
     *
     * Content-Type is set to `application/json` only if the handler
     * has not already set one — chain `setHeader('Content-Type',
     * 'application/problem+json')->json($payload)` to ship a different
     * media type.
     *
     * `$flags` is a `JSON_*` bitmask (same constants as
     * `json_encode()`). When `0`, the per-server default from
     * `HttpServerConfig::setJsonEncodeFlags()` is used —
     * `JSON_UNESCAPED_UNICODE | JSON_UNESCAPED_SLASHES` out of the box.
     *
     * `JSON_THROW_ON_ERROR` is silently stripped: encode failure
     * yields a `500` JSON error response, not a propagated exception.
     * Handlers never need to wrap `json()` in try/catch.
     *
     * @param array|string|object|int|float|bool|null $data
     * @param int $status HTTP status code, default 200
     * @param int $flags  JSON_* bitmask; 0 = use server default
     * @return static
     */
    public function json(array|string|object|null|int|float|bool $data,
                         int $status = 200,
                         int $flags = 0): static {}

    /**
     * Send HTML response.
     *
     * Sets Content-Type to text/html.
     *
     * @param string $html HTML content
     * @return static
     */
    public function html(string $html): static {}

    /**
     * Send redirect response.
     *
     * @param string $url Redirect URL
     * @param int $status HTTP status code (default: 302)
     * @return static
     */
    public function redirect(string $url, int $status = 302): static {}

    // === Send methods ===

    /**
     * End response and send to client.
     *
     * After calling end(), no more data can be written.
     *
     * @param string|null $data Optional final data to send
     */
    public function end(?string $data = null): void {}

    /**
     * Send a file as the response body. Defers actual transmission to
     * the dispose phase — this method records the path + options on
     * the response and returns immediately.
     *
     * After this call the response is sealed: every other mutating
     * method throws {@see HttpServerRuntimeException}.
     *
     * Path is treated as trusted (the handler made the access decision).
     * Errors during open / fstat (ENOENT, EACCES, oversize, non-regular)
     * surface as a 500 response since headers are not yet on the wire.
     *
     * @param string                $path    Absolute filesystem path.
     * @param SendFileOptions|null  $options Per-call options. NULL = defaults.
     */
    public function sendFile(string $path, ?SendFileOptions $options = null): void {}

    // === State methods ===

    /**
     * Check if headers have been sent.
     */
    public function isHeadersSent(): bool {}

    /**
     * Check if response is closed.
     */
    public function isClosed(): bool {}
}

// ---------------------------------------------------------------------------
// Functions
// ---------------------------------------------------------------------------

/**
 * Parse an HTTP request from a raw string (for testing).
 *
 * @param string $request Raw HTTP request
 * @return HttpRequest|false Parsed request or false on error
 */
function http_parse_request(string $request): HttpRequest|false {}

/**
 * Dispose server internal state (for testing — prevents leak detector
 * warnings). Clears the parser pool and all internal caches.
 */
function server_dispose(): void {}

}
