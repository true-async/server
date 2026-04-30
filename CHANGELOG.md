# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

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
