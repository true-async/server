# TrueAsync Server — Usage Guide

How to configure and run the server in production. The reference for what
each setter does is the inline doc on the C method; this document is the
narrative — what to wire up, in what order, why.

The entire surface lives in two classes:

| Class | Purpose |
|---|---|
| `TrueAsync\HttpServerConfig` | Pure configuration object. Mutable until handed to the server constructor; locked thereafter. |
| `TrueAsync\HttpServer`       | Runtime. Holds handlers, listeners, the event loop, and the lifecycle (`start()` / `stop()`). |

Configuration lives on the config object; handlers live on the server
object. The split exists so a config can be transferred to another worker
thread (`TrueAsync\ThreadChannel`) and reconstructed there without touching
the originating server.

---

## 1. Minimal server

```php
use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;

$server = new HttpServer(
    (new HttpServerConfig())->addListener('0.0.0.0', 8080)
);

$server->addHttpHandler(function ($request, $response) {
    $response->setStatusCode(200)->setBody('hello');
});

$server->start();   // blocks until $server->stop() is called
```

`addListener()` opens a default TCP listener accepting both HTTP/1.1 and
HTTP/2. `addHttpHandler()` registers a single callable that fires for
every request on every protocol the listener accepts. `start()` runs the
event loop on the calling thread until something calls `stop()`.

---

## 2. Listeners

A listener is a `(transport, host, port[, tls])` tuple plus a protocol
mask. The server holds up to 16 listeners and the C-level dispatcher
matches each accepted connection back to its listener row.

| Method | Transport | Accepts |
|---|---|---|
| `addListener($host, $port, $tls = false)`        | TCP | HTTP/1.1 + HTTP/2 (default) |
| `addHttp1Listener($host, $port, $tls = false)`   | TCP | HTTP/1.1 only |
| `addHttp2Listener($host, $port, $tls = false)`   | TCP | HTTP/2 only — h2c on plaintext, h2 over ALPN on TLS |
| `addHttp3Listener($host, $port)`                 | UDP/QUIC | HTTP/3 (TLS 1.3 mandatory) |
| `addUnixListener($path)`                          | Unix domain | HTTP/1.1 + HTTP/2 |

### When to pick which

- **Default (`addListener`).** Sane choice for a public-facing HTTP port.
  On TLS the protocol is selected via ALPN; on plaintext the server
  watches the first bytes — `PRI * HTTP/2.0` routes to HTTP/2, anything
  else to HTTP/1.
- **HTTP/2-only (`addHttp2Listener`).** Use for a port that must reject
  HTTP/1.1 — typical for h2c benchmark profiles, or when fronted by an
  HTTP/2-aware proxy that should never fall back. A connection that
  doesn't open with the RFC 7540 §3.5 preface gets a compliant
  `GOAWAY(PROTOCOL_ERROR)` from nghttp2 and the socket closes.
- **HTTP/1-only (`addHttp1Listener`).** Use if you want a port that
  refuses speculative HTTP/2 upgrades. A client that opens with the
  HTTP/2 preface gets a 400 from llhttp.
- **HTTP/3 (`addHttp3Listener`).** Always parallel to a TCP port, never
  in place of one. QUIC clients arrive at the UDP listener directly;
  HTTP/1.1 and HTTP/2 clients learn about it via the `Alt-Svc` header
  the server adds automatically when an H3 listener is configured.

### Common multi-listener layouts

```php
// Public port + h2c-only port for benchmarks that probe HTTP/2 behaviour.
$config
    ->addListener('0.0.0.0', 8080)            // dual H1+H2
    ->addHttp2Listener('0.0.0.0', 8082);      // h2c only

// HTTPS + HTTP/3 on the same logical service.
$config
    ->addListener('0.0.0.0', 443, tls: true)
    ->addHttp3Listener('0.0.0.0', 443)        // UDP 443, advertised via Alt-Svc
    ->setCertificate('/etc/ssl/certs/site.pem')
    ->setPrivateKey('/etc/ssl/private/site.key');
```

### Effective protocol set

The protocol the server actually accepts on a connection is
**listener mask ∩ registered handlers**. Two consequences:

- A handler-less protocol is rejected even if the listener allows it.
  Register only `addHttp2Handler()` and even a default `addListener()`
  port will refuse HTTP/1.
- A listener can narrow further than the handler set. A server with both
  `addHttpHandler` and `addHttp2Handler` registered still rejects HTTP/1
  on a port opened with `addHttp2Listener`.

---

## 3. Handlers

```php
$server->addHttpHandler(function ($req, $res) { /* … */ });   // HTTP/1.1 + HTTP/2
$server->addHttp2Handler(function ($req, $res) { /* … */ });  // HTTP/2-specific (optional)
$server->addWebSocketHandler(function ($req, $res) { /* … */ });
```

`addHttpHandler` is the common case. `addHttp2Handler` exists for two
scenarios:

1. **HTTP/2-only deployments** where you don't register an HTTP/1
   handler — the server-wide mask is then narrowed to H2 and the
   detector rejects HTTP/1 traffic.
2. **Protocol-specific dispatch** when the same port serves both H1 and
   H2 but you want an H2-aware handler (e.g. to push promises, or to
   read trailers).

A handler runs in its own coroutine on the server's TrueAsync scope.
HTTP/1 spawns one handler coroutine per request; HTTP/2 and HTTP/3
spawn one per stream. Suspending the handler (e.g. `await`) does **not**
block other connections or streams — that's the whole point.

The request object is read-only. The response object is the only
mechanism for output; the server emits the response when the handler
returns or when `$res->end()` is called explicitly.

---

## 4. TLS

Once any listener has `tls: true` (or any HTTP/3 listener exists at all),
the cert/key paths become mandatory:

```php
$config
    ->addListener('0.0.0.0', 443, tls: true)
    ->setCertificate('/etc/ssl/certs/site.pem')
    ->setPrivateKey('/etc/ssl/private/site.key');
```

A single cert/key pair is shared by every TLS listener on a server
instance. Multi-cert SNI is delegated to OpenSSL's
`tlsext_servername_cb`; configure that out-of-band if you need it.

ALPN is wired automatically — `h2,http/1.1` for TCP TLS listeners and
`h3` for UDP listeners. There's no PHP-level ALPN setter; the listener
methods you used determine which protocols are advertised.

kTLS (kernel TLS offload) is opportunistic on Linux 5.4+. There's no
config knob — the server probes after the handshake and uses it when
both directions are available.

---

## 4.5. JSON responses

`HttpResponse::json()` is the framework's standard JSON path — encodes
arrays/objects via PHP's own `php_json_encode_ex`, ships strings as-is.

```php
$server->addHttpHandler(function ($req, $resp) {
    // Array → encoded with the per-server default flags
    // (JSON_UNESCAPED_UNICODE | JSON_UNESCAPED_SLASHES out of the box).
    return $resp->json(['ok' => true, 'msg' => 'привет/мир'])->end();
});

// Pre-encoded string passthrough — no re-encoding cost. Use this when
// you have JSON cached in Redis / Memcache / a file:
$resp->json($cached_json_string)->end();

// Custom HTTP status:
$resp->json(['error' => 'invalid input'], 422)->end();

// Per-call flag override (server default is bypassed when $flags != 0):
$resp->json($data, 200, JSON_PRETTY_PRINT)->end();

// Custom Content-Type — set BEFORE json() and it is preserved.
// Useful for application/problem+json (RFC 7807),
// application/vnd.api+json (JSON:API), etc.:
$resp->setHeader('Content-Type', 'application/problem+json')
     ->json(['type' => 'about:blank', 'title' => 'oops'], 400)
     ->end();
```

Encode failure (resources, recursion limit) yields a controlled
`500 {"error":"json encoding failed"}` — handlers never need to wrap
`json()` in try/catch. `JSON_THROW_ON_ERROR` is silently stripped for
the same reason.

Per-server defaults:

```php
$config->setJsonEncodeFlags(
    JSON_UNESCAPED_UNICODE | JSON_UNESCAPED_SLASHES
);
```

---

## 5. Compression

Inbound + outbound compression with three backends — gzip (issue #8),
Brotli + zstd (issue #9). Enabled by default; the response pipeline
picks the best codec the client advertises in `Accept-Encoding`,
preferring `zstd > br > gzip`. Codecs missing from the build skip
silently.

```php
$config
    ->setCompressionEnabled(true)        // default
    ->setCompressionLevel(6)             // gzip 1..9, zlib semantics
    ->setBrotliLevel(4)                  // 0..11, default 4
    ->setZstdLevel(3)                    // 1..22, default 3
    ->setCompressionMinSize(1024)        // skip below threshold
    ->setCompressionMimeTypes([          // wholesale replacement
        'text/html', 'text/plain', 'application/json',
        'application/javascript', 'image/svg+xml',
    ])
    ->setRequestMaxDecompressedSize(10 * 1024 * 1024);   // anti zip-bomb

// Discover what this build was compiled with — useful for ops health
// checks and for skipping codec-specific tests cleanly.
$encodings = HttpServerConfig::getSupportedEncodings();
// → ["zstd", "br", "gzip", "identity"]  (order: server preference)
```

`setCompressionLevel` retains its **gzip-only** meaning; brotli and zstd
have their own ranges (see comments above) because the level scales
differ enough that linear mapping would lose the high end of either.

A handler can opt out per-response — useful for endpoints that mix
secrets with reflected user input (BREACH mitigation):

```php
$server->addHttpHandler(function ($req, $resp) {
    $resp->setNoCompression()
         ->setHeader('Content-Type', 'application/json')
         ->setBody($payload)
         ->end();
});
```

See [docs/COMPRESSION.md](COMPRESSION.md) for the full negotiation
matrix, build flags, and the H1/H2/H3-specific behaviour.

---

## 6. Timeouts and admission control

```php
$config
    ->setReadTimeout(30)        // seconds, 0 = disabled
    ->setWriteTimeout(30)
    ->setKeepAliveTimeout(5)
    ->setShutdownTimeout(5)     // grace period after stop()

    ->setBacklog(1024)          // listen(2) backlog
    ->setMaxConnections(0)      // 0 = unlimited
    ->setMaxInflightRequests(0) // 0 = derived from max_connections at start

    ->setBackpressureTargetMs(5)            // CoDel target sojourn (RFC 8289)
    ->setMaxConnectionAgeMs(0)              // proactive drain — 0 disables
    ->setMaxConnectionAgeGraceMs(0)         // hard close grace after drain signal
    ->setDrainSpreadMs(5000)                // reactive drain spread window
    ->setDrainCooldownMs(10000);            // min gap between reactive drains
```

Backpressure has two independent layers:

1. **Hard cap** — when `active_connections >= max_connections`, the
   listen socket is paused. SYNs accumulate in the kernel backlog
   instead of being accept()ed.
2. **CoDel** (off by default; set `setBackpressureTargetMs(5)` to
   enable) — samples per-request sojourn time and pauses the listener
   when min sojourn stays above target for one full window.

See [docs/RECOMMENDATIONS.md](RECOMMENDATIONS.md) for tuning guidance.

---

## 7. Observability

```php
use TrueAsync\LogSeverity;

$config
    ->setLogSeverity(LogSeverity::INFO)   // OFF / DEBUG / INFO / WARN / ERROR
    ->setLogStream(fopen('/var/log/truasync.log', 'a'))
    ->setTelemetryEnabled(true);          // ingest W3C traceparent / tracestate
```

Counters (request totals, sojourn samples, TLS handshake stats, drain
events, etc.) are exposed via `$server->getStats()` once the server is
running. Format is a flat associative array — fan it out to your
metrics backend of choice.

---

## 8. Lifecycle

```php
$server = new HttpServer($config);   // config gets locked here

$server->addHttpHandler($handler);   // OK before start()

$server->start();                    // blocks the caller until stop()

// From a coroutine, signal handler, or another thread:
$server->stop();
```

The config object is **frozen at server construction**. Setters throw
after that point; create a new `HttpServerConfig` if you need to spin
up a second server with different settings.

`start()` blocks the calling coroutine. To run the server alongside
other work, spawn it:

```php
use function Async\spawn;

$server_co = spawn(fn () => $server->start());
// … do other work, then …
$server->stop();
await($server_co);
```

`stop()` is idempotent and safe from any context. It pauses listeners,
fires the configured graceful drain, and resolves the `start()` blocker
once all in-flight handlers finish (subject to `shutdown_timeout_s`).

---

## 9. Multi-worker (built-in pool)

Set `setWorkers(N)` and `start()` spawns an internal `Async\ThreadPool`
of N workers. Each worker re-binds the same listeners; the kernel
load-balances accept() across them via `SO_REUSEPORT` on Linux.

```php
$config = (new HttpServerConfig())
    ->addListener('0.0.0.0', 8080)
    ->setWorkers(4);                    // 1 (default) = single-thread

$server = new HttpServer($config);
$server->addHttpHandler($handler);
$server->start();                       // blocks until every worker exits
```

### Caveats

- **Cross-thread shutdown is incomplete.** `$server->stop()` on the
  pool-parent throws — `Async\ThreadPool::cancel()` doesn't reliably
  wake workers suspended on their own server's wait event. Until that
  lands, terminate the process at the OS level (SIGINT / SIGTERM /
  `posix_kill`) when you need to bring everything down.
- **`SO_REUSEPORT` is Linux/BSD-only.** On Windows libuv falls back to
  a single accept thread; workers > 1 will compile but provide no
  parallelism.
- **No worker init hook.** State that's expensive to build (preloaded
  fixtures, opcache warm-up) lives in your handler closure's by-value
  captures; transfer_obj clones it once per worker. If you need an
  explicit init step that runs *before* listeners come up, fall back
  to the manual pattern in
  [`examples/multi-worker-manual.php`](../examples/multi-worker-manual.php).

---

## 10. Where to look next

- [`examples/minimal-server.php`](../examples/minimal-server.php) — bench-grade single-handler.
- [`examples/demo-server.php`](../examples/demo-server.php) — routing-style dispatch.
- [`examples/multi-worker.php`](../examples/multi-worker.php) — built-in pool via `setWorkers()`.
- [`examples/multi-worker-manual.php`](../examples/multi-worker-manual.php) — manual `Async\ThreadPool` layout (per-worker init hook).
- [`docs/COMPRESSION.md`](COMPRESSION.md) — gzip pipeline, request and response.
- [`docs/RECOMMENDATIONS.md`](RECOMMENDATIONS.md) — backpressure, drain, kernel knobs.
- [`docs/CODING_STANDARDS.md`](CODING_STANDARDS.md) — internal conventions (only relevant if you're hacking the C core).
- [`FUTURES.md`](../FUTURES.md) — outstanding C-level features needed for HttpArena production tier.
