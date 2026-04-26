# TrueAsync Server

<p align="center">
  <img src="logo.jpg" alt="TrueAsync Server" width="250"/>
</p>

High-performance HTTP/1.1, HTTP/2, and HTTP/3 server implemented as a native PHP extension,
built on top of the [TrueAsync](https://github.com/true-async) event loop.
Supports WebSocket and gRPC protocols out of the box.

## Features

- **HTTP/1.1** — full RFC 9112 compliance, keep-alive, pipelining
- **HTTP/2** — multiplexing, server push (via nghttp2)
- **HTTP/3 / QUIC** — UDP-based transport (via ngtcp2 + nghttp3)
- **TLS 1.2 / 1.3** — OpenSSL 3.x, ALPN negotiation
- **Multipart / file uploads** — streaming zero-copy parser
- **Backpressure** — CoDel (RFC 8289) adaptive pausing
- **Native coroutines** — deep integration with TrueAsync async API
- **WebSocket** — RFC 6455 compliant, upgrade from HTTP/1.1 and HTTP/2, full duplex messaging
- **SSE (Server-Sent Events)** — RFC 8895, lightweight one-way server-to-client event streaming over HTTP
- **gRPC** — built on HTTP/2, unary and streaming RPC (server/client/bidirectional)
- **Zero-copy architecture** — minimal allocations on hot paths

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

## License

Licensed under the [Apache License, Version 2.0](LICENSE).
