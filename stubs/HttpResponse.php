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
     * Get response reason phrase
     */
    public function getReasonPhrase(): string {}

    // === Header methods ===

    /**
     * Set header (replaces existing)
     *
     * Content-Length is the one header the server reads back: set before the
     * first write() it declares the length of a streamed body (see write()),
     * and on a buffered body the server states the count it is sending.
     *
     * Three fields answer differently, because the server states them itself.
     * Connection accepts only "close", which retires the connection after this
     * response — the field is not copied onto the wire, the socket is actually
     * closed. Transfer-Encoding accepts only "chunked", the framing an
     * undeclared HTTP/1.1 stream gets anyway, and is dropped; naming any other
     * coding throws, because the server cannot apply it and would otherwise
     * send encoded bytes with nothing declaring them.
     *
     * Throws {@see HttpServerInvalidArgumentException} when the name is not an
     * RFC 9110 §5.6.2 token or the value carries a byte that cannot stand in a
     * field value — a CR or an LF would end the header block and let the rest
     * be read as a second response. Nothing is stored when it throws.
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
     * A status that carries no body — 1xx, 204, 304 — throws
     * HttpServerRuntimeException here, while the response is still uncommitted
     * and can still be given a status that does carry one. A HEAD request is
     * the exception: the chunk is accepted and dropped, because the handler is
     * producing the body a GET would return.
     *
     * Parks the handler coroutine only under backpressure: HTTP/2 and HTTP/3
     * park while every ring slot is live or the queued bytes stand at
     * HttpServerConfig::setStreamWriteBufferBytes (256 KiB by default),
     * HTTP/1 parks on the socket write. tryWrite() offers a chunk without
     * committing to that wait. A peer that has gone throws HttpException 499.
     */
    public function write(string $chunk): static {}

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
     * Declare the gRPC response message encoding.
     *
     * Must be called before the first writeMessage() — the encoding rides
     * the initial HEADERS as the grpc-encoding header, and every subsequent
     * writeMessage() compresses automatically. Supported: "gzip" and
     * "identity" (the default; clears a previous declaration). Enabling
     * compression is per-call by design — a compressed message without a
     * declared encoding is a gRPC protocol error, so it cannot be expressed.
     *
     * @param string $encoding "gzip" or "identity".
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
     * True while output is still possible: end() was not called, the response
     * is not sealed by sendFile(), and the client has not gone.
     *
     * A false answer is final: stop a streaming loop on !isWritable(). For the
     * separate question of room in the outbound queue, use tryWrite() or
     * awaitWritable().
     */
    public function isWritable(): bool {}

    /**
     * True once the response has been finished, by end() or by abort().
     *
     * Reports the response, not the connection: a peer that has gone leaves
     * this false until the handler finishes the response. Use isWritable() for
     * liveness.
     */
    public function isEnded(): bool {}
}
