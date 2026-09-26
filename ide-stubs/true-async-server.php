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
 * @version    0.16.0
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

/**
 * Base class for every WebSocket error. Extends HttpServerException, so a
 * catch-all that already handles server errors keeps working unchanged.
 *
 * Also raised directly by {@see WebSocket::subscribe()} on a malformed topic
 * filter or once the connection is at its
 * {@see HttpServerConfig::setWsMaxSubscriptions()} limit, and by
 * {@see WebSocket::publish()} on a malformed topic or one carrying a wildcard.
 */
class WebSocketException extends HttpServerException
{
}

/**
 * The connection closed for a reason other than a normal peer handshake.
 * $closeCode is the RFC 6455 code (1006 Abnormal Closure when the peer left
 * without a CLOSE frame) and $closeReason the peer's UTF-8 reason text.
 *
 * A graceful peer close is NOT an exception: {@see WebSocket::recv()} returns
 * null instead.
 */
final class WebSocketClosedException extends WebSocketException
{
    public readonly int $closeCode;
    public readonly string $closeReason;
}

/**
 * Backpressure. Raised by {@see WebSocket::send()} / {@see WebSocket::sendBinary()}
 * when the outbound queue stays over the high-watermark for longer than the
 * write timeout — a slow consumer — and by {@see WebSocket::publish()} when the
 * connection is over its {@see HttpServerConfig::setWsPublishRateLimit()}.
 *
 * Either way the connection stays up: catching this is the application's cue to
 * drop the message, back off, or close.
 */
final class WebSocketBackpressureException extends WebSocketException
{
}

/**
 * Programmer error: a second coroutine called {@see WebSocket::recv()} while
 * another was already suspended in recv() on the same connection. A single byte
 * stream has no defined semantics for multiple readers, so this is rejected at
 * the boundary rather than raced. Restructure to one recv loop that dispatches.
 */
final class WebSocketConcurrentReadException extends WebSocketException
{
}

/**
 * Reliable room delivery ({@see Room::send()} / {@see HttpServer::send()})
 * failed: the retry deadline passed with a target mailbox still full, the
 * outbound queue was at its cap, or send() was called outside a coroutine.
 *
 * Delivery is at-least-once with partial delivery — the fast targets were posted
 * during fan-out, before any failure verdict. The counts say how much landed, so
 * re-sending (which duplicates on the ones that already got it) is a decision,
 * not an accident.
 *
 * Extends HttpServerException rather than WebSocketException: rooms are served
 * by a build configured with --disable-websocket, where that class does not
 * exist. A handler that caught WebSocketException around a send() catches
 * HttpServerException instead.
 *
 */
final class RoomDeliveryException extends HttpServerException
{
    /** Worker mailboxes that had accepted the message when the send failed. */
    public readonly int $delivered;

    /** Targets still unfilled when the send gave up. */
    public readonly int $pending;
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

/**
 * The IANA close-code registry (RFC 6455 §7.4.1), as accepted by
 * {@see WebSocket::close()}.
 *
 * Application-specific codes stay reachable because close() also accepts a raw
 * `int` — RFC 6455 §7.4.2 reserves 4000-4999 for exactly that.
 *
 * NO_STATUS, ABNORMAL_CLOSURE and TLS_HANDSHAKE are reserved: they describe
 * what happened, but MUST NOT be sent on the wire.
 */
enum WebSocketCloseCode: int
{
    case NORMAL                = 1000;  /* normal closure */
    case GOING_AWAY            = 1001;  /* server going down / client navigating away */
    case PROTOCOL_ERROR        = 1002;  /* protocol error */
    case UNSUPPORTED_DATA      = 1003;  /* received data of an unsupported type */
    case NO_STATUS             = 1005;  /* RESERVED — no code in the close frame */
    case ABNORMAL_CLOSURE      = 1006;  /* RESERVED — closed with no close frame */
    case INVALID_FRAME_PAYLOAD = 1007;  /* non-UTF-8 payload in a text message */
    case POLICY_VIOLATION      = 1008;  /* policy violation */
    case MESSAGE_TOO_BIG       = 1009;  /* message too large to process */
    case MANDATORY_EXTENSION   = 1010;  /* an expected extension was not negotiated */
    case INTERNAL_SERVER_ERROR = 1011;  /* unexpected server error */
    case TLS_HANDSHAKE         = 1015;  /* RESERVED — TLS handshake failure */
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
     * existence — or fall through to the PHP handler on an
     * `on_missing: NEXT` mount.
     *
     * Patterns are matched against the path *relative to the root
     * directory*, with `/` as the separator, by gitignore's rule:
     *
     *   `*.php`      a file of that name at any depth
     *   `/index.php` that file at the root directory only
     *   `cache/x`    anchored at the root directory, `*` stopping at each `/`
     *   `cache/`     a directory of that name at any depth, and all it holds
     *   `cache/**`   anchored, `*` crossing `/`
     *
     * A pattern opening with a double star names every directory, the root
     * directory among them. A pattern without `/` reads the file name, so a
     * directory named like it keeps serving what it holds — `cache/` is how to
     * cover a directory. Case follows the platform's own filesystem:
     * case-insensitive on Windows, case-sensitive elsewhere.
     *
     * @throws HttpServerInvalidArgumentException when a pattern is empty or
     *         longer than 512 bytes.
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
     * @param int $port Port to listen on, or 0 to take whatever the kernel
     *                  gives — {@see HttpServer::getBoundListeners()} reports it
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
     * Which worker answers a given connection is not promised. Where the
     * kernel has no load-balanced `SO_REUSEPORT` — macOS, the other BSDs,
     * Solaris — the workers share one listening socket and nothing arbitrates
     * between them, so a busy machine can leave a worker idle while another
     * drains the queue. Work that must be spread evenly belongs behind a queue
     * of its own, not behind the accept.
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
     * Runs only in pool mode ({@see setWorkers()} > 1), where every worker is a
     * fresh thread with nothing loaded. With a single worker the server runs in
     * the calling process, which the caller has already set up, so the closure
     * is never invoked and that setup is yours to do. Pass `null` to clear.
     *
     * @return static
     */
    public function setBootloader(?\Closure $bootloader): static {}

    /**
     * Returns the bootloader previously set, or null.
     */
    public function getBootloader(): ?\Closure {}

    /**
     * Hot-reload on file change — the development trigger. Pool mode only
     * ({@see setWorkers()} > 1).
     *
     * The pool parent watches each path recursively. A settled burst of changes
     * invalidates the watched trees in opcache and calls
     * {@see HttpServer::reload()}, so replacement workers re-run the bootloader
     * with the new code while the listen sockets stay open — no dropped
     * connection, no restart.
     *
     * @param string[] $watchPaths Directories to watch, recursively.
     * @param string[] $extensions Case-insensitive allow-list; empty = every file.
     * @param int $debounceMs Quiet window before a burst fires one reload.
     * @param int $maxHoldMs Reload at most this long after the first change
     *                       (0 = no cap), so a directory that never goes quiet
     *                       still reloads.
     * @return static
     */
    public function enableHotReload(
        array $watchPaths,
        array $extensions = ['php'],
        int $debounceMs = 300,
        int $maxHoldMs = 2000
    ): static {}

    /**
     * Hot-reload on SIGHUP — the production trigger. Pool mode only: the pool
     * parent arms a persistent SIGHUP handler that calls
     * {@see HttpServer::reload()}, which is the signal a deploy script sends
     * once the new code is on disk. Not supported on Windows.
     *
     * @return static
     */
    public function enableReloadOnSignal(bool $enabled = true): static {}

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
     * Per-stream chunk-queue cap for HttpResponse::write() backpressure.
     *
     * When the handler's write() call grows the stream's chunk queue past
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
     * Set the maximum request body size accepted on HTTP/1, HTTP/2 and
     * HTTP/3 listeners (bytes), multipart uploads included. H1 rejects with
     * 413 + connection close; H2 rejects with RST_STREAM(ENHANCE_YOUR_CALM) and
     * H3 with a stream reset, and the connection stays up for other streams; a
     * handler already running gets HttpException 413 from any read of the
     * body. HTTP/3 buffers a body in memory and holds it to 16 MiB at most.
     * In reactor-pool mode the reactor threads that receive HTTP/3 do not see
     * this setting, and every HTTP/3 body there is held to 16 MiB.
     *
     * Default: 10485760 (10 MiB). Valid: 1024 .. 17179869184 (16 GiB).
     *
     * @param int $bytes
     * @return static
     */
    public function setMaxBodySize(int $bytes): static {}

    /** @return int */
    public function getMaxBodySize(): int {}

    // === WebSocket knobs ===

    /**
     * Cap on a reassembled WebSocket message. A peer whose fragments add up to
     * more than this gets a CLOSE 1009 (Message Too Big) and the connection is
     * torn down.
     *
     * Default: 1048576 (1 MiB). Valid: 128 .. 268435456 (256 MiB).
     *
     * @return static
     */
    public function setWsMaxMessageSize(int $bytes): static {}

    /** @return int */
    public function getWsMaxMessageSize(): int {}

    /**
     * Cap on a single frame's payload. Separate from the message cap because it
     * is the fragment-flood defence: a peer that never exceeds the message size
     * can still exhaust a worker with millions of tiny fragments.
     *
     * Default: 1048576 (1 MiB). Same valid range as setWsMaxMessageSize().
     *
     * @return static
     */
    public function setWsMaxFrameSize(int $bytes): static {}

    /** @return int */
    public function getWsMaxFrameSize(): int {}

    /**
     * How many distinct topic filters one connection may hold.
     *
     * Default 0 — unlimited, which is what every self-hosted broker ships
     * (EMQX `max_subscriptions`, NATS `max_subs`): only the application knows
     * how many topics it needs.
     *
     * Set it whenever client input reaches {@see WebSocket::subscribe()} — say
     * `$ws->subscribe($msg->data)` — so a peer cannot grow the worker's topic
     * tree without end. Over the cap, subscribe() throws
     * {@see WebSocketException} and the connection stays up.
     *
     * Filter depth is capped separately and unconditionally, at 128 levels.
     *
     * @return static
     */
    public function setWsMaxSubscriptions(int $count): static {}

    /** @return int */
    public function getWsMaxSubscriptions(): int {}

    /**
     * Token bucket over {@see WebSocket::publish()}, per connection. Default 0 —
     * off, as EMQX ships its `messages_rate`.
     *
     * publish() is the one WebSocket call an unprivileged peer can turn into
     * work on *every* worker in the process — send() and trySend() only ever
     * touch its own socket. Unmetered, one client looping on a relayed message
     * fills every worker's inbox, and the drops that follow take out other
     * topics' traffic too.
     *
     * Over the rate publish() throws {@see WebSocketBackpressureException} and
     * the connection stays up — the sender is told, rather than the message
     * vanishing into a full mailbox where nobody can see it.
     *
     * @param int $perSecond Sustained publishes per second.
     * @param int $burst Bucket depth in messages — how far a handler may run
     *                   ahead of the rate. 0 = one second's worth.
     * @return static
     */
    public function setWsPublishRateLimit(int $perSecond, int $burst = 0): static {}

    /** @return int */
    public function getWsPublishRateLimit(): int {}

    /** @return int */
    public function getWsPublishBurst(): int {}

    /**
     * Reliable-send retry cadence in milliseconds — how long {@see Room::send()}
     * waits (a coroutine sleep) between retries of a target whose mailbox was
     * full. Smaller means a full target is recovered sooner, at more attempts.
     *
     * Default: 50. Applies to Room::send()/HttpServer::send() only; publish()
     * never retries.
     *
     * Read once, when the server's room machinery is created (enableRooms(), the
     * first room(), or start()), and used by every thread that joins it: the
     * drainer is one timer per worker, so there is nothing a later per-send value
     * could change.
     */
    public function setWsPublishRetryIntervalMs(int $ms): static {}

    /** @return int */
    public function getWsPublishRetryIntervalMs(): int {}

    /**
     * Default deadline in milliseconds for a reliable send: how long the outbound
     * drainer keeps retrying a still-full target before giving up — the message is
     * dropped (counted `retry_expired` in {@see HttpServer::getRuntimeStats()}),
     * and a blocking {@see Room::send()} throws. A per-call $timeoutMs overrides.
     *
     * Default: 5000.
     */
    public function setWsPublishRetryTimeoutMs(int $ms): static {}

    /** @return int */
    public function getWsPublishRetryTimeoutMs(): int {}

    /**
     * Per-worker cap on the reliable-send outbound queue. When it is full,
     * {@see Room::trySend()} returns false and {@see Room::send()} throws, parking
     * nothing — the honest bound (NATS-style) that keeps a wedged consumer from
     * growing the sender without limit.
     *
     * Default: 4096.
     */
    public function setWsPublishRetryQueueMax(int $count): static {}

    /** @return int */
    public function getWsPublishRetryQueueMax(): int {}

    /**
     * Server-initiated PING cadence (ms) on otherwise-idle connections. The peer
     * must answer with a PONG within {@see setWsPongTimeoutMs()} or the
     * connection is torn down with 1001 (Going Away).
     *
     * Default: 30000. 0 disables automatic ping.
     *
     * @return static
     */
    public function setWsPingIntervalMs(int $ms): static {}

    /** @return int */
    public function getWsPingIntervalMs(): int {}

    /**
     * How long the server waits for a PONG before declaring the connection dead.
     *
     * Default: 60000. 0 disables the timeout.
     *
     * @return static
     */
    public function setWsPongTimeoutMs(int $ms): static {}

    /** @return int */
    public function getWsPongTimeoutMs(): int {}

    /**
     * Enable permessage-deflate (RFC 7692). Off by default: it costs CPU and
     * widens the decompression-bomb surface, so it is opt-in. Negotiated only
     * when the client offers it, and the message cap is enforced both before and
     * after inflate. Requires a build with zlib (HTTP compression).
     *
     * @return static
     */
    public function setWsPermessageDeflate(bool $enabled): static {}

    /** @return bool */
    public function getWsPermessageDeflate(): bool {}

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
     * Inbound command-mailbox depth per reactor (reactor pool / HTTP/3).
     * 0 = engine default. Valid: 0, or 64 .. 1048576.
     *
     * @param int $slots
     * @return static
     */
    public function setReactorMailboxCapacity(int $slots): static {}

    /** @return int */
    public function getReactorMailboxCapacity(): int {}

    /**
     * UDP socket receive/send buffer (bytes) on HTTP/3 listeners. Absorbs
     * inbound bursts so they do not overflow into RcvbufErrors. 0 leaves the OS
     * default. The kernel clamps to net.core.{r,w}mem_max unless privileged.
     *
     * Default: 8 MiB. Valid: 0 .. 268435456 (256 MiB).
     *
     * @param int $bytes
     * @return static
     */
    public function setHttp3SocketBufferBytes(int $bytes): static {}

    /** @return int */
    public function getHttp3SocketBufferBytes(): int {}

    /**
     * Opt-in QUIC send pacing. Caps each burst at the congestion controller's
     * send_quantum and spaces packets on ngtcp2's pacing timer, which smooths
     * bulk sends over lossy or rate-limited paths.
     *
     * Default OFF: on a lossless path pacing only adds cost, so turn it on for
     * constrained-path deployments and nothing else.
     *
     * @param bool $enable
     * @return static
     */
    public function setHttp3Pacing(bool $enable): static {}

    /** @return bool */
    public function isHttp3Pacing(): bool {}

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

    // === Request scope / statistics ===

    /**
     * Per-request child scope, on by default. Turning it off reuses the
     * connection scope directly — two fewer allocations per request, at the
     * price of `Async\request_context()` returning null (reach for `?->`).
     *
     * @param bool $enable
     * @return static
     */
    public function setRequestScope(bool $enable): static {}

    /** @return bool */
    public function isRequestScope(): bool {}

    /**
     * Opt into the cross-worker statistics aggregate read by
     * {@see HttpServer::getStats()}, which throws while this is off. With it off
     * no stats slab is allocated at all. Distinct from trace-context telemetry
     * ({@see setTelemetryEnabled()}). Fixed at server start.
     *
     * @param bool $enabled
     * @return static
     */
    public function setStatsEnabled(bool $enabled): static {}

    /** @return bool */
    public function isStatsEnabled(): bool {}

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
     * Legacy toggle. WebSocket is enabled by registering a handler with
     * {@see HttpServer::addWebSocketHandler()} — there is no separate flag to
     * set. enableWebSocket(true) therefore throws, pointing you at that API;
     * enableWebSocket(false) is a no-op that stores the flag. Mirrors
     * {@see enableHttp2()}, which is enabled the same way (addHttp2Handler()).
     *
     * @param bool $enable
     * @return static
     * @throws HttpServerRuntimeException when passed true
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

    /**
     * Ciphertext-out ring — how much OpenSSL stages per SSL_write before the
     * emit path parks the tail. Larger means fewer syscalls on bodies bigger
     * than one TLS record, at roughly $bytes more memory per TLS connection;
     * smaller saves that memory with no RPS cost when responses are small.
     *
     * The value is rounded UP to whole TLS records (~17 KiB), floored at one and
     * capped at 16. 0 resets to the 64 KiB default. The getter reports the
     * effective, record-rounded size — not what you passed in.
     *
     * @param int $bytes
     * @return static
     */
    public function setTlsBufferBytes(int $bytes): static {}

    /** @return int Effective (record-rounded) ciphertext-out ring size. */
    public function getTlsBufferBytes(): int {}

    /**
     * Document root for hq-interop (HTTP/0.9-over-QUIC), which is what the QUIC
     * interop test matrix speaks. Files below it are served verbatim to hq
     * clients. No effect on h3.
     *
     * @param string $path
     * @return static
     */
    public function setHttp3HqDocroot(string $path): static {}

    /** @return string|null */
    public function getHttp3HqDocroot(): ?string {}

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
     * Fan every log record out to several destinations at once, each with its
     * own format and severity floor — a JSON access log to a file and a coloured
     * diagnostics console, side by side. Supersedes the
     * setLogSeverity()/setLogStream() single-stream sugar. At most 8 sinks;
     * a bad spec throws here, at call time, not at start().
     *
     * Each element is an array:
     *   - 'type'     => 'stream' | 'file' | 'stdout' | 'stderr' | 'syslog'   (required)
     *   - 'stream'   => resource        (required for 'stream')
     *   - 'path'     => string          (required for 'file')
     *   - 'target'   => 'tcp://host:port' | 'udp://host:port' | 'udg:///dev/log'
     *                                   (required for 'syslog')
     *   - 'facility' => 'user' | 'daemon' | 'local0'..'local7'  (syslog, default 'user')
     *   - 'format'   => 'plain' | 'logfmt' | 'json' | 'pretty' | 'template'
     *                                   (default 'plain'; ignored by syslog)
     *   - 'template' => string          (required for format 'template')
     *   - 'category' => 'app' | 'access' | 'all'   (default 'app')
     *   - 'level'    => LogSeverity     (required)
     *
     * Under a worker pool use 'file', not 'stream': each worker reopens the path
     * itself, whereas a parent-opened PHP resource cannot cross into a worker
     * thread (it is skipped there, with a notice at start).
     *
     * 'category' routes record kinds: 'app' is server diagnostics, 'access' is
     * exactly one structured record per completed request — OpenTelemetry HTTP
     * semconv attributes (http.request.method, url.path, url.query,
     * http.response.status_code, network.protocol.version,
     * http.response.body.size, http.server.request.duration, client.address,
     * client.port, plus trace context) — and 'all' is both.
     *
     * 'json' emits one OTel-Logs object per line; 'pretty' decides colour from
     * the target fd, honouring NO_COLOR / CLICOLOR_FORCE; 'syslog' emits RFC
     * 5424, octet-framed (RFC 6587) over TCP and one record per datagram on
     * udp/udg; 'template' renders a custom line — {ts} or {ts:PATTERN} with a
     * date()-style subset (Y y m d H i s v), {level}, {msg}, {attrs}, {trace},
     * {span}, everything else literal.
     *
     * No sink calls back into PHP, by design: records are emitted from IO
     * callbacks and from reactor threads that have no PHP context, so the log
     * path must never re-enter the VM. To export logs from userland, point a
     * sink at a file or socket with 'format' => 'json' and drain it from your own
     * coroutine — which also keeps exporter latency off the request path.
     *
     * @param array $sinks List of sink specs (see above).
     * @return static
     */
    public function setLogSinks(array $sinks): static {}

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
     * Add the request handler every protocol falls back to.
     *
     * HTTP/1.1, HTTP/2 and HTTP/3 all reach this handler unless a
     * protocol-specific one is registered beside it — see
     * {@see HttpServer::addHttp2Handler()} and
     * {@see HttpServer::addGrpcHandler()}.
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
     * Add the WebSocket handler. Registering it is what turns WebSocket on —
     * there is no separate switch to flip, exactly like HTTP/2 and
     * {@see HttpServer::addHttp2Handler()}. ({@see HttpServerConfig::enableWebSocket()}
     * is only a legacy toggle and throws when passed true.)
     *
     * An HTTP handler is still required: start() refuses to come up without one,
     * and it is what answers the requests that are not upgrades.
     *
     * The handler is invoked with three arguments and PHP drops the ones you did
     * not declare, so every arity works:
     *
     *   function (WebSocket $ws): void
     *   function (WebSocket $ws, HttpRequest $req): void
     *   function (WebSocket $ws, HttpRequest $req, WebSocketUpgrade $u): void
     *
     * Declare the third parameter when you need to pick a subprotocol or reject
     * the upgrade (auth) before the 101 goes out — see {@see WebSocketUpgrade}.
     *
     * It runs in its own coroutine for the life of the connection, and the server
     * closes with 1000 Normal once it returns. A handler that throws does not
     * take the worker down: the exception is logged, and the peer is told
     * in-protocol — an HTTP status if the throw beat the upgrade, a CLOSE 1011
     * once the session was live.
     *
     * @param callable $handler WebSocket handler callback
     * @return static
     */
    public function addWebSocketHandler(callable $handler): static {}

    /**
     * Enable cross-worker rooms (pub/sub topics) on this server.
     *
     * A room is a topic that any code can publish to — the message fans out to
     * every subscriber across all workers, over the same engine a WebSocket
     * connection uses with {@see WebSocket::subscribe()}. This is the only way
     * to allocate the room hub, and it must be called before start(); a build
     * configured with --disable-websocket serves rooms the same way.
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
     * @return array{served: int, posted: int, dropped: int, workers: int}
     *         Per-call delivery breakdown: `served` local subscribers on the
     *         calling worker, `posted` remote worker mailboxes that accepted the
     *         copy, `dropped` full remote mailboxes that lost it (best-effort —
     *         use send() / trySend() when a full mailbox must be retried instead
     *         of dropped), `workers` threads attached to the hub at all. See
     *         {@see Room::publish()} for what a zero `workers` means.
     */
    public function publish(string $topic, string $message, bool $binary = false): array {}

    /**
     * Reliable server-side send to a room, NON-BLOCKING. Parks a full target on
     * the outbound queue for background retry and returns at once. See
     * {@see Room::trySend()} for the full contract; this is the same without a
     * {@see Room} handle.
     *
     * @param int|null $timeoutMs Retry deadline; null uses the configured default.
     * @return bool True if delivered or parked; false if a target was left
     *         unserved with nothing parked for it — the outbound queue is full,
     *         this thread has none to park on, or the message reached nobody.
     *         False does not mean nothing was delivered: see {@see Room::trySend()}.
     */
    public function trySend(string $topic, string $message, ?int $timeoutMs = null): bool {}

    /**
     * Reliable server-side send to a room, BLOCKING. Suspends the calling
     * coroutine until delivered or the deadline passes, then THROWS. See
     * {@see Room::send()} for the full contract; this is the same without holding
     * a {@see Room} handle.
     *
     * @param int|null $timeoutMs Retry deadline; null uses the configured default.
     * @return int Targets the message reached, always 1 or more: subscribers
     *             served on the calling worker plus worker mailboxes that accepted
     *             it. See {@see Room::send()} for what that number is and is not.
     * @throws RoomDeliveryException if the deadline passed with a target still
     *         full, the outbound queue was full, the call was made outside a
     *         coroutine, or the message reached nobody.
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
     * Add a handler for connections that speak HTTP/2.
     *
     * Takes precedence over {@see HttpServer::addHttpHandler()} on such a
     * connection; a gRPC call registered through
     * {@see HttpServer::addGrpcHandler()} still wins over both. Registered
     * alone it also narrows the server-wide protocol mask to HTTP/2, so
     * HTTP/1 traffic is refused.
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
     * On a pool parent ({@see HttpServerConfig::setWorkers()} > 1) it retires the
     * whole cohort and SUSPENDS until the server is really down — when it
     * returns, the workers have drained, the pool is torn down and the listen
     * sockets are closed. Call it from a coroutine; a `Async\signal(SIGTERM)`
     * handler is the usual place.
     *
     * A standalone server's stop() does not suspend: it is normally called from
     * a request handler, and the shutdown drain waits on that very handler — so
     * a blocking stop() there would be waiting for itself.
     *
     * @return bool True if stopped successfully
     */
    public function stop(): bool {}

    /**
     * Hot-reload the worker pool. Pool parent only.
     *
     * Workers finish what they are holding, stop and exit; fresh worker threads
     * re-run the bootloader — picking up the changed code — and take over on the
     * same listen sockets, so no connection is refused across the swap. Suspends
     * until the old cohort has drained; start() keeps running throughout.
     *
     * Invalidate the changed files first (opcache_invalidate) or rely on opcache
     * timestamp validation, otherwise the new workers compile the old code.
     *
     * Usually you do not call this yourself — wire a trigger instead:
     * {@see HttpServerConfig::enableHotReload()} (watch files, for development)
     * or {@see HttpServerConfig::enableReloadOnSignal()} (SIGHUP, for a deploy).
     *
     * @return bool True when every replacement worker was resubmitted; false if
     *              a reload is already running or a replacement failed.
     */
    public function reload(): bool {}

    /**
     * Check if server is running.
     */
    public function isRunning(): bool {}

    /**
     * The listeners the server actually holds, one entry per configured
     * listener, in configuration order.
     *
     * A listener configured with port 0 is bound to a port the kernel picks,
     * and the entry carries that port: there is no gap between choosing an
     * address and owning it, which a caller picking a free port beforehand
     * cannot avoid. HTTP/3 listeners still require an explicit port and
     * report the configured one.
     *
     * Empty while the server is not running: before start(), after stop().
     *
     * @return array<int, array{type: 'tcp'|'udp_h3', host: string, port: int, tls: bool}
     *                  |array{type: 'unix', path: string}>
     */
    public function getBoundListeners(): array {}

    /**
     * Whether the extension was built with HTTP/2 support (--enable-http2).
     */
    public static function isHttp2(): bool {}

    /**
     * Whether the extension was built with HTTP/3 support (--enable-http3).
     */
    public static function isHttp3(): bool {}

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
     *  - `ws_bodies_freed`      — and released. Bodies are persistent memory
     *                             owned by whatever holds them — a mailbox, a
     *                             receiver's ring, a parked retry — so a teardown
     *                             that forgets to empty one of those leaks them.
     *                             At rest, with nothing queued anywhere, this
     *                             equals `ws_bodies`; a standing gap is that leak.
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
     * Cross-worker request statistics.
     *
     * Opt-in — throws unless {@see HttpServerConfig::setStatsEnabled()} was on
     * before start(). Returns:
     *
     *   [
     *     'enabled'  => true,
     *     'workers'  => [ <id> => ['total_requests' => …, …], … ],
     *     'reactors' => [ … ],   // requests served entirely on a transport reactor
     *     'totals'   => ['total_requests' => …, …],   // folded across both
     *   ]
     *
     * `totals` carries `total_requests`, the per-class `responses_2xx/3xx/4xx/5xx_total`
     * (each request is classified exactly once, so the four sum to
     * `total_requests`), the live gauges `conns_active_h1/h2/h3`, and
     * `log_records_dropped_total`.
     *
     * Each counter is combined the way its meaning allows: monotonic totals sum
     * and survive a {@see reload()} (a retiring worker's totals are inherited, so
     * a scraper never sees a counter run backwards just because the pool
     * rotated); active gauges sum across live workers only. Reads are lock-free,
     * so the aggregate can be stale by at most one worker mid-rotation.
     *
     * @return array
     */
    public function getStats(): array {}
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
     * Returns empty string if no body. A multipart body is not kept on HTTP/1
     * and HTTP/2: its parts go to {@see getPost()} and {@see getFiles()}.
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
     * The client's IP address — `"203.0.113.7"`, `"2001:db8::1"`.
     *
     * A bare IP: no port, and no brackets around an IPv6 literal. That is the
     * shape of `$_SERVER['REMOTE_ADDR']` (RFC 3875 §4.1.8), so it feeds straight
     * into filter_var(…, FILTER_VALIDATE_IP), an ACL, or a rate limiter. The
     * port is {@see getRemotePort()}.
     *
     * NULL on a Unix-socket listener, which has no IP peer.
     *
     * This is the peer of the TCP/QUIC connection. It is NOT derived from
     * X-Forwarded-For — behind a proxy, parse that header yourself, and only when
     * you trust the proxy that set it.
     */
    public function getRemoteAddress(): ?string {}

    /**
     * The client's port, e.g. 54321. NULL when there is no IP peer.
     */
    public function getRemotePort(): ?int {}

    /**
     * Form fields of an application/x-www-form-urlencoded or multipart/form-data body.
     *
     * Keys follow PHP's rules for $_POST: name[] appends, user[name] and
     * matrix[0][1] nest, and a `.` or a space in the base name becomes `_`.
     * Empty for any other Content-Type.
     *
     * A form over a limit is refused rather than shortened. A body past
     * {@see HttpServerConfig::setMaxBodySize()} is refused by the transport:
     * HTTP/1 answers 413 before the handler runs, HTTP/2 and HTTP/3 reset the
     * stream, and the handler, already running, gets {@see HttpException} 413
     * from every read of the body while the client gets no status. More
     * multipart fields than max_input_vars, or a malformed multipart body, is
     * refused with 400 where the parser meets it: HTTP/1 answers before the
     * handler runs, and HTTP/2 resets the stream as for an oversized body.
     * HTTP/3 buffers the body and parses it in this getter, so there, as for
     * an url-encoded form past max_input_vars or a name nested deeper than
     * max_input_nesting_level on every transport, this getter throws
     * HttpException 400, and so does every later form getter; uncaught, it
     * answers the request with 400. The body itself stays readable then.
     *
     * Over HTTP/2 and HTTP/3 the handler starts before the body has arrived, so
     * this call suspends until it has, as {@see awaitBody()} does. A form body
     * does not stream even with {@see HttpServerConfig::setBodyStreamingEnabled()},
     * and reading it with {@see readBody()} leaves the form in place.
     *
     * @return array
     */
    public function getPost(): array {}

    /**
     * Uploaded files of a multipart/form-data body, keyed as in {@see getPost()}:
     * ['avatar' => UploadedFile, 'photos' => [UploadedFile, ...], 'docs' => ['cv' => UploadedFile]].
     * Waits for the body, and is refused, as getPost() is.
     *
     * @return array
     */
    public function getFiles(): array {}

    /**
     * Get single uploaded file by name.
     * For an array of files (photos[], docs[cv]), returns the first file directly
     * in it; deeper arrays are read through {@see getFiles()}.
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
     * Pulls one chunk from the per-request queue produced by the H1/H2
     * parsers when HttpServerConfig::setBodyStreamingEnabled(true) was
     * set at server start. Suspends the current coroutine until a chunk
     * is available, then returns a non-empty string. Returns null
     * idempotently at end of stream.
     *
     * Each call returns exactly one parser-supplied chunk (an H2 DATA
     * frame payload or one llhttp on_body slice). $maxLen is reserved
     * for a future coalescing optimisation and is ignored today.
     *
     * @param int $maxLen Maximum bytes to return (default 65536).
     * @return string|null Next chunk, or null at end of stream.
     * @throws HttpException with the status of a refusal on HTTP/2 or HTTP/3:
     *         413 past {@see HttpServerConfig::setMaxBodySize()}, 400 for a
     *         refused multipart body. A read already parked here wakes with it.
     * @throws \Exception if the body stream broke otherwise (a peer reset, a
     *         lost connection).
     */
    public function readBody(int $maxLen = 65536): ?string {}

    // === gRPC ===

    /**
     * Deframe the next gRPC message from the request body.
     *
     * Extracts one 5-byte-length-prefixed message and advances an internal
     * cursor, so call it once for a unary RPC and loop it for client-streaming.
     * Returns null once no complete message remains. What you get back is the
     * raw protobuf; decode it in userland (ext/protobuf).
     *
     * @return string|null Next message, or null when none remains.
     * @throws \Exception if a framed message exceeds the size limit.
     */
    public function readMessage(): ?string {}

    /**
     * The call deadline from the `grpc-timeout` header, in (fractional) seconds,
     * or null when the client sent none.
     *
     * The server does not abort the handler on it — the client enforces its own
     * deadline — but a handler can honour it against its own operations.
     */
    public function getGrpcTimeout(): ?float {}
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
     * Takes 200 to 599. An interim status (1xx) throws: RFC 9110 §15.2 makes
     * it a response the client reads and then goes on waiting for the final
     * one, which a handler has no way to send afterwards.
     *
     * A status that carries no content changes what the body calls do. 204,
     * 304 and 205 refuse a streaming call and drop a buffered body; 205 states
     * Content-Length: 0, the other two state no length at all.
     *
     * @param int $code HTTP status code (200-599)
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
     * The phrase sits on the HTTP/1 status line, where RFC 9112 §4 allows
     * HTAB, SP, VCHAR and obs-text and nothing else. Every other byte is
     * replaced with a space: a CR or an LF would end the status line early and
     * let the rest be read as header fields. HTTP/2 and HTTP/3 carry no reason
     * phrase and ignore this.
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
     * Content-Length is the one header the server reads back: set before the
     * first write() it declares the length of a streamed body (see write()),
     * and on a buffered body the server states the count it is sending.
     *
     * Two fields answer differently, because the server states them itself.
     * Connection is read rather than copied: "close" retires the connection
     * after this response on HTTP/1 — the field is not copied onto the wire,
     * the socket is closed — while HTTP/2 and HTTP/3 multiplex, so one response
     * never retires their connection and the request is recorded and unused
     * there. "keep-alive" is dropped, being what the server was going to say
     * anyway; any other value throws. Transfer-Encoding accepts only "chunked",
     * the framing an undeclared HTTP/1.1 stream gets anyway, and is dropped;
     * naming any other coding throws, because the server cannot apply it and
     * would otherwise send encoded bytes with nothing declaring them.
     *
     * getHeader() reports neither afterwards: what the server states is not
     * part of the handler's header set. resetHeaders() takes back a close.
     *
     * Throws {@see HttpServerInvalidArgumentException} when the name is not an
     * RFC 9110 §5.6.2 token, or the value carries a byte that cannot stand in a
     * field value — a CR or an LF would end the header block and let the rest
     * be read as a second response — or the value has leading or trailing
     * whitespace, which §5.5 forbids a sender to generate. The bytes checked
     * are the bytes stored, so a value given as an object is checked after its
     * __toString(). Nothing is stored when it throws.
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
     * Stream a chunk to the client.
     *
     * The first call commits status and headers; afterwards setStatusCode(),
     * setHeader() and setBody() throw. Later calls append chunked-transfer
     * segments (HTTP/1.1) or DATA frames (HTTP/2, HTTP/3). To append to a
     * buffered body instead, call appendBody().
     *
     * A Content-Length set before this first call frames the body instead of
     * chunks, and the server then holds the body to it: a chunk that would
     * pass the declared count throws HttpServerRuntimeException and is not
     * queued, and a body that ends short of it is failed rather than finished.
     * Such a response is never compressed.
     *
     * An HTTP/1.0 client gets neither: it has no chunked decoder, so an
     * undeclared body reaches it as its own bytes with Connection: close, and
     * the close is the boundary. The connection carries that one response.
     *
     * A status that carries no body — 204, 304, 205 — throws
     * HttpServerRuntimeException here, while the response is still uncommitted
     * and can still be given a status that does carry one. A HEAD request is
     * the exception: the chunk is accepted and dropped, and the response stays
     * uncommitted, so setHeader() and setStatusCode() go on working and an
     * uncaught exception still becomes the status. What the dropped chunk does
     * change is the length: the server states none, because a count taken from
     * the buffer nobody filled would claim the GET body is empty. Set a
     * Content-Length to state the length a GET would report.
     *
     * Parks the handler coroutine only under backpressure: HTTP/2 and HTTP/3
     * park while every ring slot is live or the queued bytes stand at
     * HttpServerConfig::setStreamWriteBufferBytes (256 KiB by default),
     * HTTP/1 parks on the socket write. tryWrite() offers a chunk without
     * committing to that wait. A peer that has gone throws HttpException 499.
     */
    public function write(string $chunk): static {}

    /**
     * Removed. One bool answered four questions, and a loop that read it as
     * liveness stopped streams that were merely slow.
     *
     * Ask the two questions separately: isWritable() reports whether output is
     * still possible, tryWrite() and awaitWritable() report whether the
     * outbound queue has room.
     *
     * The declaration stays for one minor release so a call names its
     * replacements instead of failing as an undefined method.
     *
     * @throws HttpServerRuntimeException always
     */
    public function sendable(): bool {}

    /**
     * Offer a chunk without waiting for room: false means the outbound queue
     * had no room and nothing was queued, so the same chunk can be offered
     * again later. The transport answers at the moment of queueing, not from
     * a predicate read beforehand, so nothing slips in between.
     *
     * A client that has gone is not reported as false — it throws
     * HttpException 499, because "wait" and "stop" need opposite reactions.
     * The refused chunk is a slice of one byte stream, so dropping it corrupts
     * the body: retry it, or stop.
     *
     * HTTP/1 is the exception, and it is not a small one: that transport keeps
     * no queue of its own, so it never refuses AND an accepted chunk waits for
     * the socket for as long as a blocking write() would — up to the write
     * timeout. A handler
     * that must not be parked has to check getProtocolVersion(). Over HTTP/2,
     * HTTP/3 and the worker pool neither happens.
     */
    public function tryWrite(string $chunk): bool {}

    /**
     * Wait until the outbound queue has room again, and report whether it has.
     *
     * The companion to tryWrite(): that call says "not now", this one waits for
     * "now" instead of spinning. The wait belongs to the transport, which keeps
     * its own deadline and re-pumps its drain on each wake.
     *
     * True at once on HTTP/1, which keeps no queue and so has nothing to wait
     * for. False without waiting on a transport that can be full but offers no
     * wait — better than "go ahead", which would spin a handler that trusts it.
     * A timeout or a cancellation arrives as an exception; false after a wait
     * means the queue is still full.
     *
     * @param int|null $timeoutMs Milliseconds to wait. The shorter of this and
     *                            the connection's write timeout bounds the
     *                            wait; null leaves that timeout as the only
     *                            bound.
     */
    public function awaitWritable(?int $timeoutMs = null): bool {}

    /**
     * True while output is still possible: end() was not called, the response
     * is not sealed by sendFile(), and the client has not gone.
     *
     * A false answer is final: stop a streaming loop on !isWritable(). For the
     * separate question of room in the outbound queue, use tryWrite() or
     * awaitWritable().
     */
    public function isWritable(): bool {}

    // === Server-Sent Events ===

    /**
     * Switch the response into Server-Sent Events mode and lock the headers.
     *
     * Sets the three canonical SSE headers — `Content-Type:
     * text/event-stream`, `Cache-Control: no-cache, no-transform` and
     * `X-Accel-Buffering: no` (the last tells nginx not to buffer the
     * response; without it events stall behind the proxy buffer until it
     * fills) — and marks the response as not-compressible (a buffering
     * gzip stream would defeat real-time delivery). The response then
     * enters streaming mode exactly as the first {@see self::write()} would:
     * status + headers are committed and may no longer change, but no event
     * data is emitted until the first sseEvent()/sseComment().
     *
     * Calling sseStart() is optional — the first sseEvent()/sseComment()
     * starts the stream implicitly. Note that sseStart() alone does NOT
     * flush the status line / headers onto the wire: the commit is lazy and
     * happens on the first sseEvent()/sseComment()/sseRetry() (or, if none
     * is ever sent, an empty `200 text/event-stream` is flushed when the
     * response ends). To open the stream eagerly — e.g. to unblock the
     * browser's `onopen` before any real event is ready — send an initial
     * `sseComment()` (the conventional `:\n\n` prelude), which both starts
     * the stream and puts the headers on the wire immediately.
     *
     * Throws {@see HttpServerInvalidArgumentException} if the handler has
     * already set a Content-Type other than `text/event-stream`, and
     * {@see HttpServerRuntimeException} if the response is already
     * streaming, closed, has no connection to stream over, or carries a status
     * that ends at the header block (1xx, 204, 304), where an event stream has
     * no body to put its records in.
     *
     * @return static
     */
    public function sseStart(): static {}

    /**
     * Send one SSE record. Multiline `$data` is split into one `data:` field per
     * line (WHATWG §9.2 framing), and the record is terminated by a blank line
     * so the browser dispatches it at once. `$event`, `$id` and `$retry` are
     * emitted only when non-null.
     *
     * `$event` and `$id` must contain no `\r` or `\n` — the parser would read
     * those as field/record separators — and `$id` must contain no NUL, which
     * would make the parser drop the id entirely.
     *
     * `$data === ""` is valid and dispatches an empty MessageEvent. All four
     * arguments null is a no-op. An event carrying neither `data` nor `retry` is
     * dropped by the EventSource parser.
     *
     * @param string|null $data  Payload. Multiline strings are split.
     * @param string|null $event Event name (what addEventListener() matches).
     * @param string|null $id    Event id — echoed as Last-Event-ID on reconnect.
     * @param int|null    $retry Reconnect-delay hint, ms.
     * @throws HttpServerInvalidArgumentException on a newline in $event/$id, a
     *         NUL in $id, or a negative $retry.
     * @return static
     */
    public function sseEvent(
        ?string $data = null,
        ?string $event = null,
        ?string $id = null,
        ?int $retry = null
    ): static {}

    /**
     * Dispatch one SSE event without waiting for room.
     *
     * The non-blocking twin of sseEvent(): the same record, the same field
     * validation, the same start of the stream on the first call. False means
     * the outbound queue is full — the record was not queued and no header was
     * committed, so the same event may be offered again. A peer that is gone
     * throws the 499 exception instead, as tryWrite() does. All four arguments
     * null is a no-op and answers true.
     *
     * HTTP/1 never refuses: it keeps no queue of its own, so an accepted record
     * waits for the socket as a blocking one would.
     *
     * @param string|null $data  Message payload. Multiline strings are split.
     * @param string|null $event Event name (matched by addEventListener()).
     * @param string|null $id    Event id — echoed as Last-Event-ID on reconnect.
     * @param int|null    $retry Reconnect delay hint in milliseconds.
     */
    public function trySseEvent(
        ?string $data = null,
        ?string $event = null,
        ?string $id = null,
        ?int $retry = null
    ): bool {}

    /**
     * Send an SSE comment — a record beginning with `:`.
     *
     * Browsers ignore comments, which is exactly what makes them the heartbeat:
     * they keep the connection alive past an intermediary's idle timeout (nginx
     * `proxy_read_timeout`, 60 s by default). The canonical payload is the empty
     * string, which goes on the wire as `:\n\n`. Starts the stream if it is not
     * already running.
     *
     * `$text` must contain no `\r` or `\n`.
     *
     * @return static
     */
    public function sseComment(string $text = ""): static {}

    /**
     * Send a bare `retry:` directive — how long the browser should wait before
     * reconnecting after the stream drops. Sugar for sseEvent(retry: $ms) with no
     * payload. Starts the stream if it is not already running.
     *
     * @param int $milliseconds Non-negative reconnect-delay hint.
     * @return static
     */
    public function sseRetry(int $milliseconds): static {}

    // === gRPC ===

    /**
     * Declare the response message encoding, before the first
     * {@see writeMessage()} — it rides the initial HEADERS as `grpc-encoding`,
     * and every writeMessage() after it compresses automatically.
     *
     * Supported: `"gzip"` and `"identity"` (the default; clears a previous
     * declaration). Compression is a declaration rather than a per-message flag
     * by design: a compressed message with no declared encoding is a gRPC
     * protocol error, so the API does not let you express one.
     *
     * @return static
     */
    public function setGrpcEncoding(string $encoding): static {}

    /**
     * Frame and stream one gRPC message.
     *
     * Prepends the 5-byte gRPC length prefix to $message and streams it as
     * a single gRPC message. Activates streaming mode on the first call,
     * exactly like write(). Call once for a unary reply, repeatedly for
     * server-streaming. Pass the already protobuf-encoded bytes; the
     * grpc-status is carried separately via setTrailer() (defaults to 0
     * when unset). Compressed automatically when setGrpcEncoding('gzip')
     * was declared.
     *
     * @param string $message Protobuf-encoded message bytes.
     * @return static
     */
    public function writeMessage(string $message): static {}

    /**
     * Frame and stream one gRPC message without waiting for room.
     *
     * The non-blocking twin of writeMessage(): the same framing, the same
     * declared grpc-encoding, the same switch into streaming mode on the first
     * call. False means the outbound queue is full — nothing was queued and no
     * header was committed, so the same message may be offered again. A peer
     * that is gone throws the 499 exception instead, as tryWrite() does.
     *
     * HTTP/1 never refuses: it keeps no queue of its own, so an accepted
     * message waits for the socket as a blocking one would.
     *
     * @param string $message Protobuf-encoded message bytes.
     */
    public function tryWriteMessage(string $message): bool {}

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
     */
    public function setBody(string $body): static {}

    /**
     * Append to the buffered response body.
     *
     * Nothing reaches the client here: the whole body goes out on end(), with
     * Content-Length computed from it. Call write() to stream instead — that
     * is the call which commits headers and applies backpressure.
     */
    public function appendBody(string $data): static {}

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
     * Finish a started stream as failed, so the client can tell a body that
     * stopped from a body that finished.
     *
     * HTTP/1 writes no terminating chunk and loses the connection — chunked
     * framing has no other way to say it, and curl reports CURLE_PARTIAL_FILE.
     * HTTP/2 sends RST_STREAM and HTTP/3 resets the stream, leaving the rest of
     * the connection alone.
     *
     * $errorCode is the reset code of whichever protocol carries the response,
     * and it does not travel between them: HTTP/2 and HTTP/3 number the same
     * conditions differently, and HTTP/1 has no field for one. Omitted, each
     * transport uses its own INTERNAL_ERROR.
     *
     * A response that never started streaming has nothing to disown and is left
     * alone: the exception the handler is carrying goes on to become the
     * status. A stream started with nothing yet on the wire is finished
     * cleanly instead — the client gets the empty response the transport
     * commits for it. Calling abort() twice is a no-op for the same reason
     * neither of those throws: its place is a catch block, where a method that
     * throws buries the handler's own error.
     *
     * @param int|null $errorCode Protocol reset code, 0..4294967295. Omitted:
     *                             the transport's own INTERNAL_ERROR.
     * @throws HttpServerRuntimeException if end() has already told the client
     *         the body is whole.
     * @throws HttpServerInvalidArgumentException if $errorCode is out of range.
     */
    public function abort(?int $errorCode = null): void {}

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
     * True once the response has been finished, by end() or by abort().
     *
     * Reports the response, not the connection: a peer that has gone leaves
     * this false until the handler finishes the response. Use isWritable() for
     * liveness.
     */
    public function isEnded(): bool {}
}

// ---------------------------------------------------------------------------
// WebSocket
// ---------------------------------------------------------------------------

/**
 * One fully-reassembled message, as handed back by {@see WebSocket::recv()}.
 * Text messages were UTF-8 validated by the framing layer, so `$data` can be
 * used as-is — there is nothing left to re-check.
 */
final class WebSocketMessage
{
    /**
     * The payload. Valid UTF-8 for a text message.
     */
    public readonly string $data;

    /**
     * True when the peer sent a Binary frame (opcode 0x2), false for Text (0x1).
     */
    public readonly bool $binary;

    /**
     * Constructed by the server; user code receives these from recv().
     */
    private function __construct() {}
}

/**
 * A handle on the upgrade that has not committed yet — it exists from the moment
 * the handler is invoked until either reject() is called or the handler returns,
 * at which point the 101 goes out carrying whatever setSubprotocol() picked.
 *
 * You only get one by declaring the third parameter:
 *
 *   $server->addWebSocketHandler(
 *       function (WebSocket $ws, HttpRequest $req, WebSocketUpgrade $u): void { … }
 *   );
 *
 * The arity is read by Reflection at registration; the two-argument form skips
 * this object and accepts the upgrade with default settings.
 *
 * Once the handshake commits, every method here throws — Sec-WebSocket-Protocol
 * is already on the wire and a subprotocol cannot be unsaid.
 */
final class WebSocketUpgrade
{
    private function __construct() {}

    /**
     * Refuse the upgrade: no 101, the connection answers with $status and closes.
     * This is where authentication belongs. Return from the handler afterwards —
     * no further I/O is permitted.
     *
     * @param int $status HTTP status; must be 4xx or 5xx.
     * @param string $reason Optional response body.
     */
    public function reject(int $status, string $reason = ''): void {}

    /**
     * Choose a subprotocol from the client's offers; the token is echoed back in
     * Sec-WebSocket-Protocol. Must be called before reject() and before the
     * handler returns.
     *
     * The token is NOT re-validated against {@see getOfferedSubprotocols()} —
     * picking an offer the client actually made is the caller's job.
     */
    public function setSubprotocol(string $name): void {}

    /**
     * @return string[] Tokens from Sec-WebSocket-Protocol, in the client's order
     * of preference. Empty when it offered none.
     */
    public function getOfferedSubprotocols(): array {}

    /**
     * @return string[] Raw offers from Sec-WebSocket-Extensions, in client order.
     * permessage-deflate (RFC 7692) is negotiated for you when
     * {@see HttpServerConfig::setWsPermessageDeflate()} is on; everything else
     * here is informational. Empty when the client offered none.
     */
    public function getOfferedExtensions(): array {}
}

/**
 * One WebSocket connection. The server creates it the moment the handshake
 * commits and passes it as the first argument to the handler registered with
 * {@see HttpServer::addWebSocketHandler()}.
 *
 * Lifecycle
 * ---------
 * The connection is bound to the handler coroutine: when the handler returns —
 * for any reason, including `return` out of the recv loop on a null — the server
 * closes with 1000 Normal. Call close() explicitly only when you need a
 * different code or a reason string.
 *
 * Concurrency
 * -----------
 * - send() / sendBinary() / ping() are safe from any coroutine on the same
 *   thread. Producers enqueue whole serialized frames atomically and a single
 *   cooperative flusher writes them out one at a time, so frames cannot
 *   interleave on the wire.
 * - recv() is single-reader: a second concurrent recv() throws
 *   {@see WebSocketConcurrentReadException}. One byte stream has no meaning for
 *   two readers.
 * - close() is idempotent and callable from anywhere.
 *
 * `foreach ($ws as $msg)` is the recv() loop written the other way round.
 */
final class WebSocket implements \Iterator
{
    /**
     * Constructed by the server.
     */
    private function __construct() {}

    /**
     * Receive the next text or binary message, suspending until one arrives or
     * the connection closes.
     *
     * @return WebSocketMessage|null A message, or null when the peer closed
     * cleanly — a normal CLOSE code (1000/1001/1005), or a plain disconnect with
     * no CLOSE frame at all. Hence the usual loop:
     * `while (($m = $ws->recv()) !== null) { … }`.
     *
     * @throws WebSocketClosedException on a protocol error or an explicit error
     *         close code; its readonly $closeCode / $closeReason carry the RFC
     *         6455 code and the peer's reason text.
     * @throws WebSocketConcurrentReadException if another coroutine is already
     *         blocked in recv() on this connection.
     */
    public function recv(): ?WebSocketMessage {}

    /**
     * Send a text frame. The data MUST be valid UTF-8 — invalid UTF-8 is rejected
     * here, at the boundary, so the receiver never sees a frame that breaks RFC
     * 6455 §5.6.
     *
     * Returns immediately while the outbound queue is under the high-watermark,
     * which is the common case. Over it, the calling coroutine suspends until
     * drain brings the queue back down — and if that suspension outlasts the
     * write timeout, throws {@see WebSocketBackpressureException}, leaving the
     * handler to drop the message, close, or retry.
     *
     * @throws WebSocketBackpressureException on a prolonged drain stall.
     * @throws WebSocketClosedException if the connection is already closed.
     */
    public function send(string $text): void {}

    /**
     * Send a binary frame — no UTF-8 constraint. Backpressure semantics are
     * identical to {@see send()}.
     */
    public function sendBinary(string $data): void {}

    /**
     * Non-blocking send. Queues a text frame and returns true while the outbound
     * queue is under the high-watermark; at or over it, returns false WITHOUT
     * queueing, so the caller can drop the message, slow down, or close. Never
     * suspends — which is what makes it the right tool for a broadcast loop,
     * where one slow client must not stall delivery to everyone else.
     *
     * The high-watermark is {@see HttpServerConfig::setStreamWriteBufferBytes()}
     * (0 = disabled → trySend always queues and returns true).
     *
     * @return bool true if accepted, false if backpressured.
     * @throws WebSocketClosedException if the connection is already closed.
     */
    public function trySend(string $text): bool {}

    /**
     * Non-blocking binary send. @see trySend()
     *
     * @return bool true if accepted, false if backpressured.
     * @throws WebSocketClosedException if the connection is already closed.
     */
    public function trySendBinary(string $data): bool {}

    /**
     * Send a PING; RFC 6455 §5.5.2 requires the peer to answer with a PONG.
     * Handlers rarely need this — the server's keepalive timer
     * ({@see HttpServerConfig::setWsPingIntervalMs()}) pings on its own.
     *
     * @param string $payload Up to 125 bytes (RFC 6455 §5.5).
     */
    public function ping(string $payload = ''): void {}

    /**
     * Start the close handshake and tear the connection down. Idempotent.
     *
     * @param WebSocketCloseCode|int $code A standard code through the enum, or a
     *        raw int in 4000-4999 (application-specific, RFC 6455 §7.4.2).
     * @param string $reason UTF-8 reason text, up to 123 bytes — the close
     *        payload is 125, minus 2 for the code.
     */
    public function close(
        WebSocketCloseCode|int $code = WebSocketCloseCode::NORMAL,
        string $reason = ''
    ): void {}

    /**
     * True once close() has been called, or the peer's CLOSE frame processed.
     */
    public function isClosed(): bool {}

    /**
     * The subprotocol negotiated during the upgrade, or null if none was chosen.
     */
    public function getSubprotocol(): ?string {}

    /**
     * The peer's IP address — bare, like {@see HttpRequest::getRemoteAddress()}:
     * no port, no brackets around an IPv6 literal. NULL on a Unix-socket
     * listener, which has no IP peer.
     */
    public function getRemoteAddress(): ?string {}

    /**
     * The peer's port. NULL when there is no IP peer.
     */
    public function getRemotePort(): ?int {}

    // === Topics — publish/subscribe across every worker ===
    //
    // A worker is a thread with its own PHP context, so an array of connections
    // could only ever reach the peers of one worker — which is why a chat used to
    // need setWorkers(1). Topics live in the server instead: each worker indexes
    // the connections it owns, and a publish is handed to every worker, which
    // delivers to its own sockets. No Redis, no single-worker server.
    //
    // A topic is addressed by NAME, at the call site. There is no topic object to
    // obtain, hold, or pass into a handler.
    //
    // Filters follow MQTT: `/` separates levels, `+` matches exactly one level,
    // and a trailing `#` matches the rest. So `user/42/#` receives both
    // `user/42/presence` and `user/42`, and `order/+/status` receives the status
    // of an order that did not exist when you subscribed.

    /**
     * Subscribe this connection to a topic filter. Idempotent.
     *
     * @param string $filter May contain `+` / `#` wildcards.
     * @throws WebSocketException on a malformed filter, or once the connection
     *         holds its {@see HttpServerConfig::setWsMaxSubscriptions()} limit.
     */
    public function subscribe(string $filter): void {}

    /**
     * Drop a filter. Idempotent — one never subscribed to is a no-op. A closing
     * connection unsubscribes from everything by itself.
     */
    public function unsubscribe(string $filter): void {}

    /**
     * The filters this connection holds, in no particular order.
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
     * @return int Subscribers served on the CALLING worker — connections and
     *         server-side receivers alike. Delivery to the other workers is
     *         asynchronous and cannot be counted here, so this is a local
     *         number, not a process-wide one.
     * @throws WebSocketException on a malformed topic, or one carrying a wildcard.
     * @throws WebSocketBackpressureException when the connection is over its
     *         HttpServerConfig::setWsPublishRateLimit(). The connection stays up.
     */
    public function publish(string $topic, string $text, bool $excludeSelf = true): int {}

    /**
     * Binary counterpart of {@see publish()}.
     */
    public function publishBinary(string $topic, string $data, bool $excludeSelf = true): int {}

    /**
     * Subscribers across all workers that a publish to $topic would reach — a
     * WebSocket connection, or a server-side receiver that called
     * {@see Room::subscribe()} — including those subscribed through a wildcard
     * that matches it.
     *
     * Each worker answers with its own count and the answers are summed, so this
     * is a snapshot rather than a live number: a worker that does not answer in
     * time is left out.
     */
    public function subscriberCount(string $topic): int {}

    // === Iterator === so `foreach ($ws as $msg)` mirrors a recv() loop. The
    // cursor advances by pulling the next message; iteration ends on a graceful
    // close and throws WebSocketClosedException on an error close.

    public function current(): ?WebSocketMessage {}
    public function key(): int {}
    public function next(): void {}
    public function rewind(): void {}
    public function valid(): bool {}
}

// ---------------------------------------------------------------------------
// Rooms
// ---------------------------------------------------------------------------

/**
 * A server-side handle to a room (a pub/sub topic), obtained from
 * {@see HttpServer::room()}.
 *
 * Publishing through it reaches every subscriber of the topic across all
 * workers — a coroutine that called {@see Room::subscribe()}, or, in a build
 * with WebSocket, a connection that called `WebSocket::subscribe()` — with no
 * sending connection, so nobody is excluded. A room needs no connection at all:
 * a background producer can push into one, and a build configured with
 * --disable-websocket serves rooms the same way.
 *
 * A handle owns a reference to the topic hub, so it keeps publishing after the
 * {@see HttpServer} that minted it is released.
 *
 */
final class Room
{
    /* Rooms are minted by HttpServer::room(), never with `new`. */
    private function __construct() {}

    /**
     * Publish a text message to this room (best-effort, no retry).
     *
     * @return array{served: int, posted: int, dropped: int, workers: int}
     *         Per-call delivery breakdown: `served` local subscribers on the
     *         calling worker, `posted` remote worker mailboxes that accepted the
     *         copy, `dropped` full remote mailboxes that lost it, `workers`
     *         threads attached to this room's hub at all. Delivery to other
     *         workers is asynchronous, so `served` is a local count, not a total.
     *
     *         `workers` is what tells a publish that reached nobody why: 0 means
     *         nothing was running to receive it and no later attach can rescue
     *         this message; a non-zero `workers` with served+posted == 0 means
     *         the workers are there and the room is simply empty.
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
     * @return bool True if delivered outright or parked for retry; false if some
     *         target was left unserved and nothing was parked for it — the
     *         outbound queue is at {@see HttpServerConfig::setWsPublishRetryQueueMax()},
     *         or this thread has no outbound queue to park on, or the message
     *         reached nobody at all (no worker attached, or nobody subscribed).
     *
     *         False is NOT a promise that nothing was delivered: the fan-out runs
     *         before the refusal, so the fast targets may already hold the
     *         message and a re-send duplicates on them. The eventual outcome of a
     *         PARKED message is in {@see HttpServer::getRuntimeStats()}.
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
     * A send that reaches NOBODY throws rather than returning 0: on this path a
     * message that arrived nowhere is a failure, and the two reasons — nothing is
     * running, or nobody has joined the room — are told apart by the message,
     * because they are fixed differently. Use {@see publish()} for a message that
     * may legitimately reach no one.
     *
     * A cancellation of the calling coroutine says nothing about the message: it
     * stays on the retry queue until it lands or expires, and its outcome is then
     * only in {@see HttpServer::getRuntimeStats()}. Re-sending in a cancellation
     * handler duplicates.
     *
     * @param int|null $timeoutMs Retry deadline; null uses
     *        {@see HttpServerConfig::setWsPublishRetryTimeoutMs()}.
     * @return int Targets the message reached, always 1 or more: subscribers
     *             served on the calling worker plus worker mailboxes that accepted
     *             it. Not a subscriber census and not comparable between senders —
     *             a remote worker is one target however many subscribers sit
     *             behind it, and a mailbox that accepted the message can still
     *             drop it on a full ring (`ws_sub_overflow`).
     * @throws RoomDeliveryException if the deadline passed with a target still
     *         full, or the outbound queue was full at enqueue, or send() was
     *         called outside a coroutine (use trySend() there), or the message
     *         reached nobody.
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
