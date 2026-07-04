<?php

/**
 * @generate-class-entries
 */

namespace TrueAsync;

/**
 * HTTP Response (fluent interface)
 * @strict-properties
 * @not-serializable
 */
final class HttpResponse
{
    /**
     * Private constructor - instances created internally by server
     */
    private function __construct() {}

    // === Status methods ===

    /**
     * Set response status code
     *
     * @param int $code HTTP status code (100-599)
     * @return static
     */
    public function setStatusCode(int $code): static {}

    /**
     * Get response status code
     */
    public function getStatusCode(): int {}

    /**
     * Set response reason phrase
     *
     * @param string $phrase Reason phrase (e.g., "OK", "Not Found")
     * @return static
     */
    public function setReasonPhrase(string $phrase): static {}

    /**
     * Get response reason phrase
     */
    public function getReasonPhrase(): string {}

    // === Header methods ===

    /**
     * Set header (replaces existing)
     *
     * @param string $name Header name
     * @param string|array $value Header value(s)
     * @return static
     */
    public function setHeader(string $name, string|array $value): static {}

    /**
     * Add header value (appends to existing)
     *
     * @param string $name Header name
     * @param string|array $value Header value(s)
     * @return static
     */
    public function addHeader(string $name, string|array $value): static {}

    /**
     * Check if header exists
     *
     * @param string $name Header name (case-insensitive)
     */
    public function hasHeader(string $name): bool {}

    /**
     * Get header value (first value if multiple)
     *
     * @param string $name Header name (case-insensitive)
     * @return string|null Header value or null if not exists
     */
    public function getHeader(string $name): ?string {}

    /**
     * Get header line (all values comma-separated)
     *
     * @param string $name Header name (case-insensitive)
     */
    public function getHeaderLine(string $name): string {}

    /**
     * Get all headers
     *
     * @return array Headers with all values
     */
    public function getHeaders(): array {}

    /**
     * Reset all headers
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
     * Get protocol name (always "HTTP")
     */
    public function getProtocolName(): string {}

    /**
     * Get protocol version (e.g., "1.1", "2")
     */
    public function getProtocolVersion(): string {}

    // === Body methods ===

    /**
     * Write data to response body buffer
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
     * Frame and stream one gRPC message.
     *
     * Prepends the 5-byte gRPC length prefix (identity encoding) to
     * $message and streams it as a single gRPC message. Activates
     * streaming mode on the first call, exactly like send(). Call once for
     * a unary reply, repeatedly for server-streaming. Pass the already
     * protobuf-encoded bytes; the grpc-status is carried separately via
     * setTrailer() (defaults to 0 when unset).
     *
     * @param string $message Protobuf-encoded message bytes.
     * @return static
     */
    public function writeMessage(string $message): static {}

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
     * Get current body content
     */
    public function getBody(): string {}

    /**
     * Set body content (replaces buffer)
     *
     * @param string $body Body content
     * @return static
     */
    public function setBody(string $body): static {}

    /**
     * Get body stream (TODO)
     *
     * @return mixed Stream resource or null
     */
    public function getBodyStream(): mixed {}

    /**
     * Set body stream (TODO)
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
     * @param array|string|object|scalar|null $data
     * @param int $status HTTP status code, default 200
     * @param int $flags  JSON_* bitmask; 0 = use server default
     * @return static
     */
    public function json(array|string|object|null|int|float|bool $data,
                         int $status = 200,
                         int $flags = 0): static {}

    /**
     * Send HTML response
     *
     * Sets Content-Type to text/html
     *
     * @param string $html HTML content
     * @return static
     */
    public function html(string $html): static {}

    /**
     * Send redirect response
     *
     * @param string $url Redirect URL
     * @param int $status HTTP status code (default: 302)
     * @return static
     */
    public function redirect(string $url, int $status = 302): static {}

    // === Send methods ===

    /**
     * End response and send to client
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

    // === Server-Sent Events (text/event-stream) ===

    /**
     * Switch the response into Server-Sent Events mode and lock the headers.
     *
     * Sets the three canonical SSE headers — `Content-Type:
     * text/event-stream`, `Cache-Control: no-cache, no-transform` and
     * `X-Accel-Buffering: no` (the last tells nginx not to buffer the
     * response; without it events stall behind the proxy buffer until it
     * fills) — and marks the response as not-compressible (a buffering
     * gzip stream would defeat real-time delivery). The response then
     * enters streaming mode exactly as the first {@see self::send()} would:
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
     * streaming, closed, or has no connection to stream over.
     *
     * @return static
     */
    public function sseStart(): static {}

    /**
     * Format and send one Server-Sent Event, starting the stream if needed.
     *
     * Multiline `$data` is split on `\n` / `\r\n` / `\r` and emitted as one
     * `data:` field per line (WHATWG §9.2 event-stream framing). `$event`,
     * `$id` and `$retry` are emitted only when non-null. The record is
     * terminated by a blank line so the browser dispatches it immediately.
     *
     * `$event` and `$id` must not contain `\r` or `\n` (the parser would
     * read them as field/record separators) and `$id` must not contain NUL
     * (WHATWG: a NUL makes the parser ignore the whole id) — violations
     * throw {@see HttpServerInvalidArgumentException}. `$retry` must be
     * non-negative.
     *
     * Empty `$data === ""` is valid and dispatches an empty MessageEvent.
     * All four arguments null is a no-op. Note the EventSource parser drops
     * an event carrying neither `data` nor `retry`.
     *
     * @param string|null $data  Message payload. Multiline strings are split.
     * @param string|null $event Event name (matched by addEventListener()).
     * @param string|null $id    Event id — echoed as Last-Event-ID on reconnect.
     * @param int|null    $retry Reconnect delay hint in milliseconds.
     * @return static
     */
    public function sseEvent(
        ?string $data = null,
        ?string $event = null,
        ?string $id = null,
        ?int $retry = null
    ): static {}

    /**
     * Send an SSE comment line (a record beginning with `:`).
     *
     * Browsers ignore comments, but they keep the connection alive past
     * intermediary idle timeouts (nginx `proxy_read_timeout`, default 60s).
     * Call periodically as a heartbeat — the canonical payload is the empty
     * string, which becomes `:\n\n` on the wire. Starts the stream if it is
     * not already running.
     *
     * `$text` must not contain `\r` or `\n`.
     *
     * @param string $text Optional comment payload (informational only).
     * @return static
     */
    public function sseComment(string $text = ""): static {}

    /**
     * Send a bare `retry:` directive telling the browser how long to wait
     * before reconnecting after the stream drops, in milliseconds. Sugar
     * for sseEvent(retry: $milliseconds) with no message payload. Starts
     * the stream if it is not already running.
     *
     * @param int $milliseconds Non-negative reconnect delay hint.
     * @return static
     */
    public function sseRetry(int $milliseconds): static {}

    // === State methods ===

    /**
     * Check if headers have been sent
     */
    public function isHeadersSent(): bool {}

    /**
     * Check if response is closed
     */
    public function isClosed(): bool {}
}
