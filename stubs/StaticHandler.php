<?php

/**
 * @generate-class-entries
 */

namespace TrueAsync;

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
 *
 * @strict-properties
 * @not-serializable
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
