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
     * chunked-transfer segments (HTTP/1, Phase 2). Blocks the
     * handler coroutine ONLY when the per-stream queue crosses
     * HttpServerConfig::setStreamWriteBufferBytes (default 256 KiB);
     * otherwise returns immediately.
     *
     * HTTP/1 path lands in Phase 2 — currently throws on HTTP/1.
     *
     * @param string $chunk
     * @return static
     */
    public function send(string $chunk): static {}

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
     * Send JSON response
     *
     * Sets Content-Type to application/json and encodes data
     *
     * @param mixed $data Data to encode
     * @return static
     */
    public function json(mixed $data): static {}

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

    // === Server-Sent Events (text/event-stream) ===

    /**
     * Switch the response into SSE mode and commit headers.
     *
     * Sets `Content-Type: text/event-stream`, `Cache-Control: no-cache,
     * no-transform` and `X-Accel-Buffering: no` (the last one tells nginx
     * not to buffer the response — without it events stall behind the
     * proxy buffer until it fills). Then commits the response just like
     * the first send() would: status + headers go on the wire, no DATA
     * frame is emitted yet.
     *
     * If the handler has already set a Content-Type to anything other
     * than text/event-stream, throws — sseStart() is an explicit switch
     * into SSE and a conflicting type indicates a bug.
     *
     * After sseStart() the response is in streaming mode; setStatusCode,
     * setHeader, setBody, write, json, html, redirect all throw.
     *
     * @return static
     */
    public function sseStart(): static {}

    /**
     * Format and send one Server-Sent Event.
     *
     * Multiline `$data` is split on `\n`/`\r\n`/`\r` and emitted as one
     * `data:` field per line (per WHATWG §9.2). `$event`, `$id` and
     * `$retry` are emitted only if non-null. The block is terminated with
     * a blank line.
     *
     * `$event` and `$id` must not contain `\r` or `\n` — the stream parser
     * would interpret them as field terminators. Throws
     * InvalidArgumentException if they do.
     *
     * Empty `$data === ""` is valid (the browser dispatches an empty
     * MessageEvent). `null` `$data` is also valid for cases like sending
     * a `retry:` directive without an accompanying message — note that
     * an event with neither `data` nor `retry` is dropped by the
     * EventSource parser.
     *
     * @param string|null $data    Message payload. Multiline strings are split.
     * @param string|null $event   Event name (for addEventListener).
     * @param string|null $id      Event ID — surfaced by the browser as
     *                             Last-Event-ID on reconnect.
     * @param int|null    $retry   Reconnect delay hint, in milliseconds.
     * @return static
     */
    public function sseEvent(
        ?string $data = null,
        ?string $event = null,
        ?string $id = null,
        ?int $retry = null
    ): static {}

    /**
     * Send an SSE comment line (starts with `:`).
     *
     * Browsers ignore comments, but they keep the TCP connection alive
     * past intermediary idle timeouts (nginx's `proxy_read_timeout`,
     * default 60s). Call periodically as a heartbeat — the canonical
     * payload is the empty string, which becomes `:\n\n` on the wire.
     *
     * Comment text must not contain `\r` or `\n`.
     *
     * @param string $text Optional comment payload (informational only).
     * @return static
     */
    public function sseComment(string $text = ""): static {}

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
