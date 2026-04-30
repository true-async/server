<?php

/**
 * @generate-class-entries
 * @strict-properties
 */

namespace TrueAsync;

/**
 * HTTP Request representation (read-only)
 */
final class HttpRequest
{
    /**
     * Private constructor - instances created internally by server
     */
    private function __construct() {}

    /**
     * Get HTTP method (GET, POST, PUT, DELETE, etc.)
     */
    public function getMethod(): string {}

    /**
     * Get request URI (path + query string)
     */
    public function getUri(): string {}

    /**
     * Get HTTP version string (e.g., "1.1")
     */
    public function getHttpVersion(): string {}

    /**
     * Check if header exists (case-insensitive)
     */
    public function hasHeader(string $name): bool {}

    /**
     * Get single header value by name (case-insensitive)
     * Returns null if header doesn't exist
     */
    public function getHeader(string $name): ?string {}

    /**
     * Get header line (all values comma-separated)
     * Returns empty string if header doesn't exist
     */
    public function getHeaderLine(string $name): string {}

    /**
     * Get all headers as associative array
     * Header names are lowercase
     *
     * @return array<string, string>
     */
    public function getHeaders(): array {}

    /**
     * Get request body
     * Returns empty string if no body
     */
    public function getBody(): string {}

    /**
     * Check if request has a body
     */
    public function hasBody(): bool {}

    /**
     * Check if connection should be kept alive
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
     * Get Content-Type header value
     * Returns null if not set
     */
    public function getContentType(): ?string {}

    /**
     * Get Content-Length header value
     * Returns null if not set or invalid
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
}
