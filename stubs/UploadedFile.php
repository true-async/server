<?php

/**
 * @generate-class-entries
 * @strict-properties
 */

namespace TrueAsync;

/**
 * Represents an uploaded file (PSR-7 compatible)
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
