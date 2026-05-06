# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Fixed

- Proactive drain mis-fired on the first response when CoDel/telemetry
  were disabled. The fallback timestamp used `CLOCK_MONOTONIC_COARSE`
  while `created_at_ns` and `drain_not_before_ns` are `zend_hrtime`
  (CLOCK_MONOTONIC_RAW); the two domains drift by minutes after
  suspend / NTP slewing. Drain check now stays in the same clock
  domain in both H1 dispose and H2 commit paths.

### Added

- Per-listener protocol mask (FUTURES #1). New
  `HttpServerConfig::addHttp1Listener()` / `addHttp2Listener()` for
  protocol-restricted ports (h2c-only, h1-only). Default
  `addListener()` unchanged (H1+H2).
- Built-in worker pool (issue #11). New `HttpServerConfig::setWorkers(N)`
  spawns an internal `Async\ThreadPool` from `start()`; each worker
  re-binds the listeners (`SO_REUSEPORT`). Default `1` keeps current
  behaviour bit-for-bit. Cross-thread `stop()` is a follow-up.
- `HttpResponse::json(array|string $data, int $status = 200, int $flags = 0)`
  — explicit JSON serialization through `php_json_encode_ex`. String
  passthrough for pre-encoded payloads (cached JSON ships as-is).
  Custom Content-Type set via `setHeader()` before `json()` is
  preserved (works for `application/problem+json`,
  `application/vnd.api+json`, etc.). Encode failure yields a
  controlled `500 {"error":"json encoding failed"}` — no exception
  propagation; `JSON_THROW_ON_ERROR` silently stripped. Per-server
  default flags via `HttpServerConfig::setJsonEncodeFlags()`,
  defaulting to `JSON_UNESCAPED_UNICODE | JSON_UNESCAPED_SLASHES`.
- HTTP body compression phase 2 (issue #9): Brotli (`br`) + zstd
  (`zstd`) plug into the phase-1 `http_encoder_t` vtable. Preference
  order `zstd > br > gzip > identity`; inbound `Content-Encoding: br`
  / `zstd` decoded under the same anti-bomb cap as gzip. New setters
  `setBrotliLevel(int)` (0..11, default 4), `setZstdLevel(int)`
  (1..22, default 3); `setCompressionLevel` stays gzip-only. Build
  flags `--enable-brotli` / `--enable-zstd` (auto-detect; missing libs
  warn + skip codec, build still succeeds). New introspection method
  `HttpServerConfig::getSupportedEncodings()`. Compression sources
  also wired into `config.w32` (Windows) and `CMakeLists.txt`.
  Compression coverage 80.4% → 82.9% lines.
- `docs/USAGE.md` — full configuration guide.

## [0.3.2] - 2026-05-06

### Fixed

- **Windows build broken in 0.3.0–0.3.1.** The bailout-firewall log
  line included `<sys/syscall.h>` and called `syscall(SYS_gettid)`
  unconditionally for the thread-id field — both POSIX-only, so MSVC
  failed with `fatal error C1083: Cannot open include file:
  'sys/syscall.h'`. The include is now guarded by `#ifdef _WIN32`
  (using `<windows.h>` on Windows), and the thread-id lookup uses
  `GetCurrentThreadId()` on Windows / `syscall(SYS_gettid)` elsewhere.
  Linux glibc and musl behaviour is unchanged.

## [0.3.1] - 2026-05-06

### Fixed

- **Alpine / musl libc build broken in 0.3.0.** The bailout-firewall
  diagnostic added in 0.3.0 included `<execinfo.h>` and called
  `backtrace()` / `backtrace_symbols_fd()` unconditionally — both are
  glibc-only, so musl-based images (Alpine) failed to compile with
  `fatal error: execinfo.h: No such file or directory`. The header
  is now gated behind a new `HAVE_EXECINFO_H` autoconf check
  (`AC_CHECK_HEADERS`) and the C-stack dump compiles only on
  platforms that have it. On musl / Windows the bailout fence still
  emits the PHP-level `zend_error` line via SAPI; the C-frame dump
  is silently skipped (no fake "unavailable" notice in stderr).

## [0.3.0] - 2026-05-06

### Added

- **Bailout firewall at H1/H2/H3 request boundary.** PHP fatal errors
  thrown from a user handler (E_ERROR, OOM, uncaught exceptions during
  shutdown) no longer take the server process down. Each protocol's
  request entry point now wraps the handler call in a bailout fence
  that drains the failing coroutine, emits a 500, and lets the listener
  keep accepting. Same behaviour across HTTP/1.1, HTTP/2 streams and
  HTTP/3 streams.
- **HTTP body compression** — gzip on responses and inbound request
  bodies, served identically across HTTP/1.1, HTTP/2 and HTTP/3.
  Build flag: `--enable-http-compression` (default on; auto-detects
  zlib-ng with system zlib as fallback).

  Five `HttpServerConfig` setters drive the policy and are frozen at
  `HttpServer::__construct`:
  - `setCompressionEnabled(bool)` — master switch (default `true`).
  - `setCompressionLevel(int)` — zlib level 1..9 (default 6).
  - `setCompressionMinSize(int)` — body-size threshold below which
    responses stay identity (default 1 KiB; valid 0..16 MiB).
  - `setCompressionMimeTypes(array)` — replaces the whitelist
    wholesale (nginx semantics). Default ships the union of nginx
    `gzip_types` and h2o text-only defaults.
  - `setRequestMaxDecompressedSize(int)` — anti-zip-bomb cap on
    decoded request bodies (default 10 MiB; 0 = no cap, must be
    explicit).

  Per-response opt-out: `HttpResponse::setNoCompression()` overrides
  every other rule. Use for endpoints combining secrets with
  reflected user input (BREACH mitigation), pre-encoded payloads,
  or anywhere the server must not wrap the body.

  Negotiation follows RFC 9110 §12.5.3 — q-values, `identity;q=0`,
  `*;q=0` excludes identity unless an explicit identity entry
  rescues it. Default when no `Accept-Encoding` header is sent
  resolves to identity-only (matches nginx; safer than the strict
  RFC reading). Skip rules: status 1xx/204/304, HEAD, Range
  responses, handler-set `Content-Encoding`, MIME outside the
  whitelist, body below the threshold.

  Inbound: `Content-Encoding: gzip` (and the legacy `x-gzip`
  alias) on requests is decoded transparently. `identity` is a
  no-op. Unknown codings → 415; bomb-cap exceeded → 413; corrupt
  inflate → 400. The handler observes the decoded body via
  `HttpRequest::getBody()`.

  Streaming: when handlers call `HttpResponse::send($chunk)`, the
  compressing wrapper transparently engages on first call (subject
  to negotiation) and produces one downstream chunk per source
  chunk — preserving framing efficiency on chunked H1 and H2
  DATA frames.

  Backend: `zlib-ng` is preferred at build time for ~2-4× higher
  throughput at the same compression level; system `zlib` is the
  drop-in fallback. Both share the same source via a thin
  `zng_*` ↔ `*` macro layer.

  Issue [#8](https://github.com/true-async/server/issues/8).

### Changed

- **HTTP/2 enabled by default in the build.** `--enable-http2` (Linux
  `config.m4`) and `ARG_ENABLE('http2', …)` (Windows `config.w32`) now
  default to `yes` (auto-detected via `libnghttp2 ≥ 1.57`). Previously
  the default was `no`, so a vanilla `./configure --enable-http-server`
  produced a binary whose TLS listener advertised only `http/1.1` over
  ALPN — h2 was silently absent. Use `--disable-http2` to opt out.
  Mirrors the existing HTTP/3 default.

### Fixed

- **CoDel backpressure misfired on HTTP/2 multiplexing.** The default
  CoDel queue-management hook applied per-connection sojourn estimates
  to muxed h2 streams, where short fast streams would push the
  connection into an "overloaded" state and pause unrelated long-lived
  streams. CoDel is now off by default; opt in explicitly when wanted.

## [0.2.0] - 2026-05-04

### Added

- **`HttpRequest::getPath()`** — returns the URI path without the query
  string (e.g. `/search` from `/search?q=hello`). Works identically for
  HTTP/1.1, HTTP/2 (`:path` pseudo-header), and HTTP/3.
- **`HttpRequest::getQuery(): array`** — returns all query parameters as
  an associative array, equivalent to `$_GET`. Supports percent-decoding,
  `+`-as-space, and PHP array notation (`foo[]`, `foo[bar]`).
- **`HttpRequest::getQueryParam(string $name, mixed $default = null): mixed`** —
  returns a single query parameter by name, or `$default` (null unless
  overridden) when the parameter is absent.

  All three methods share a single lazy parse: the URI is split into path
  and query string on the first call and the result is cached in the
  request struct for subsequent accesses. The query parser delegates to
  `php_default_treat_data(PARSE_STRING, …)` — the same function PHP uses
  to populate `$_GET`.

### Fixed

- **Cross-thread `HttpServer` transfer dropped requests.** When an
  `HttpServer` was passed into a worker thread (e.g. via `Async\ThreadPool`),
  `http_server_transfer_obj()` copied the registered handlers into
  `protocol_handlers` but did not mirror the corresponding
  `view.protocol_mask` bits. `detect_and_assign_protocol()` consults the mask
  to dispatch parsed requests, so worker threads bound the listen socket and
  parsed incoming bytes but silently dropped every request as if no handler
  were registered. The transfer path now sets the same mask bits that
  `addHttpHandler` / `addHttp2Handler` / `addWebSocketHandler` /
  `addGrpcHandler` set at registration time. Regression test:
  `tests/phpt/server/core/007-server-transfer-handler-dispatch.phpt`.

## [0.1.0] - 2026-04-30

Initial public release of TrueAsync Server — a native PHP extension that runs a
high-performance HTTP/1.1, HTTP/2, and HTTP/3 server directly inside PHP, built
on the [TrueAsync](https://github.com/true-async) event loop.

### Added

- **HTTP/1.1** — full RFC 9112 implementation with keep-alive and pipelining,
  built on the vendored [`llhttp`](deps/llhttp/UPSTREAM.md) 9.3.0 parser (same
  parser used by Node.js).
- **HTTP/2** — multiplexing and server push via `libnghttp2` (>= 1.57, with the
  rapid-reset mitigation floor for CVE-2023-44487).
- **HTTP/3 / QUIC** — UDP transport via `libngtcp2` + `libnghttp3`, using the
  OpenSSL 3.5 QUIC TLS API (`libngtcp2_crypto_ossl` backend). All ten ship-gates
  of the HTTP/3 plan landed: transport, TLS 1.3, request/response, streaming,
  lifecycle + drain, `Alt-Svc` advertisement, compliance smoke, and fuzzing.
- **TLS 1.2 / 1.3** — OpenSSL 3.x with ALPN negotiation, weak cipher suites
  disabled, stateless session tickets, safe renegotiation disabled.
- **Multi-protocol on a single port** — HTTP/1.1, HTTP/2, and (planned)
  WebSocket / SSE / gRPC share the same TCP listener; protocol selection via
  ALPN or HTTP `Upgrade`. HTTP/3 runs on the same UDP port and is advertised
  through `Alt-Svc`.
- **Multipart / file uploads** — streaming zero-copy parser.
- **Backpressure** — CoDel (RFC 8289) adaptive pausing on the read path.
- **Native coroutine integration** — deep integration with the TrueAsync async
  API, including the `udp_bind` hook required for HTTP/3.
- **Zero-copy architecture** — minimal allocations on hot paths.
- **Single-threaded event-loop model** — one thread owns connection and request
  end-to-end; horizontal scaling via `SO_REUSEPORT` worker processes.
- **Cross-platform builds** — Linux, macOS, and Windows (PHP-SDK / `nmake`).
  Note: HTTP/3 outbound batching uses Linux `UDP_SEGMENT` (GSO); Windows HTTP/3
  throughput is lower as a result.
- **Build system** — `config.m4` / `config.w32` / `CMakeLists.txt` with
  `--enable-http-server`, `--enable-http2`, `--enable-http3`, `--enable-tests`
  (cmocka), `--enable-coverage`, and `--without-openssl` toggles.
- **Test suite** — PHPT integration tests under `tests/phpt/` (~124 tests),
  cmocka unit tests, and a fuzzing harness under `fuzz/`.
- **CI** — extension loaded under the correct name, `run-tests.php` wired up,
  coverage baseline tracked, CodeQL analysis configuration
  (`codeql-analyze.php`).
- **Security posture** — dedicated audit covering HTTP parsing edge cases, TLS
  configuration, memory safety, and protocol-level attack vectors (HTTP request
  smuggling, HPACK bombing, QUIC amplification). Hot paths exercised under
  AddressSanitizer and Valgrind in CI.
- **Documentation** — `README.md` (overview, architecture, install for Linux
  and Windows, quick start), `docs/` (coding standards, contributor
  recommendations, llhttp upstream notes), Apache 2.0 `LICENSE`.

[Unreleased]: https://github.com/true-async/server/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/true-async/server/releases/tag/v0.1.0
