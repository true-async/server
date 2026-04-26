<p align="center">
  <img src="logo.jpg" alt="TrueAsync Server" width="250"/>
</p>

<h1 align="center">TrueAsync Server</h1>

<p align="center">
  High-performance HTTP/1.1, HTTP/2, and HTTP/3 server as a native PHP extension,<br/>
  built on the <a href="https://github.com/true-async">TrueAsync</a> event loop.
</p>

<p align="center">
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-Apache%202.0-blue.svg" alt="License"/></a>
  <img src="https://img.shields.io/badge/PHP-8.6%2B-blue.svg" alt="PHP 8.6+"/>
  <img src="https://img.shields.io/badge/HTTP-1.1%20%7C%202%20%7C%203-green.svg" alt="HTTP 1.1 | 2 | 3"/>
  <img src="https://img.shields.io/badge/TLS-1.2%20%7C%201.3-green.svg" alt="TLS 1.2 | 1.3"/>
  <img src="https://img.shields.io/badge/WebSocket-RFC%206455-orange.svg" alt="WebSocket"/>
  <img src="https://img.shields.io/badge/gRPC-supported-orange.svg" alt="gRPC"/>
  <img src="https://img.shields.io/badge/security-audited-brightgreen.svg" alt="Security Audited"/>
</p>

---

## Features

| Status | Feature | Details |
|--------|---------|---------|
| ✅ Ready | **HTTP/1.1** | Full RFC 9112 compliance, keep-alive, pipelining |
| ✅ Ready | **TLS 1.2 / 1.3** | OpenSSL 3.x, ALPN negotiation |
| ✅ Ready | **Multipart / file uploads** | Streaming zero-copy parser |
| ✅ Ready | **Backpressure** | CoDel (RFC 8289) adaptive pausing |
| ✅ Ready | **Native coroutines** | Deep integration with TrueAsync async API |
| ✅ Ready | **Zero-copy architecture** | Minimal allocations on hot paths |
| ✅ Ready | **HTTP/2** | Multiplexing, server push (via nghttp2) |
| 🔄 In progress | **HTTP/3 / QUIC** | UDP-based transport (via ngtcp2 + nghttp3) |
| 🔄 In progress | **WebSocket** | RFC 6455, upgrade from HTTP/1.1 and HTTP/2, full duplex |
| 📋 Planned | **SSE (Server-Sent Events)** | RFC 8895, server-to-client event streaming |
| 📋 Planned | **gRPC** | Built on HTTP/2, unary and streaming RPC |

### Development Progress

```
HTTP/1.1   ████████████████████  100%
TLS        ████████████████████  100%
HTTP/2     ████████████████████  100%
HTTP/3     ████████░░░░░░░░░░░░   40%
WebSocket  ██████░░░░░░░░░░░░░░   30%
SSE        ░░░░░░░░░░░░░░░░░░░░    0%
gRPC       ░░░░░░░░░░░░░░░░░░░░    0%
```

---

## Architecture

TrueAsync Server follows the **single-threaded event loop** model — the same approach used by
[NGINX](https://nginx.org), [Envoy](https://www.envoyproxy.io), [Node.js](https://nodejs.org),
and Rust's [Tokio](https://tokio.rs)/[hyper](https://hyper.rs).

The core idea: **one thread owns both the connection and the request from start to finish**.
There is no handoff between an "accept thread" and a "worker thread", no lock contention,
no context switch between the two. A single event loop receives the connection, reads bytes off
the socket, parses the HTTP stream, dispatches the request to user code, and writes the response
— all without leaving the thread.

```
         ┌─────────────────────────────────────────┐
         │              Event Loop Thread          │
         │                                         │
  accept ─►  parse  ─►  dispatch  ─►  respond      │
         │     ▲                        │          │
         │     └──── coroutine yield ◄──┘          │
         └─────────────────────────────────────────┘
```

Non-blocking I/O is handled by the **libuv reactor** (via TrueAsync). When a coroutine needs to
wait for I/O — reading a file, querying a database, waiting for the next WebSocket frame — it
yields back to the event loop, which immediately picks up the next ready event. No thread sits
idle waiting.

To scale across CPU cores, multiple worker processes are launched (one per core) with
`SO_REUSEPORT`, so the kernel distributes incoming connections across them. Each process runs its
own fully independent event loop — no shared state, no global locks.

This model delivers predictable latency, low memory footprint under high concurrency, and
near-linear horizontal scaling.

---

## Security

Security is a first-class concern in TrueAsync Server.

- **Security audit** — the codebase has undergone a dedicated security analysis covering HTTP parsing edge cases, TLS configuration, memory safety, and protocol-level attack vectors (HTTP request smuggling, HPACK bombing, QUIC amplification)
- **Memory safety** — all hot paths are tested with AddressSanitizer and Valgrind; zero memory leaks policy enforced in CI
- **TLS hardened** — TLS 1.2/1.3 only, weak cipher suites disabled, stateless session tickets, safe renegotiation disabled
- **HTTP/3 security** — QUIC amplification limits and connection ID rotation implemented per RFC 9000 recommendations

If you discover a security vulnerability, please report it privately via GitHub Security Advisories.

---

## Requirements

- PHP 8.6+
- TrueAsync extension
- OpenSSL 3.x (optional, for TLS)
- nghttp2 (optional, for HTTP/2)
- ngtcp2 + nghttp3 (optional, for HTTP/3)

## Installation

```bash
phpize
./configure --enable-http-server
make
make install
```

Enable in `php.ini`:

```ini
extension=http_server
```

## Quick Start

```php
$server = new TrueAsync\Server\HttpServer(
    host: '0.0.0.0',
    port: 8080,
);

$server->onRequest(function ($request, $response) {
    $response->end('Hello, World!');
});

$server->start();
```

---

## License

Licensed under the [Apache License, Version 2.0](LICENSE).
