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

## 3.5. The response body

A body is produced in one of three modes. The first body call fixes the mode
for that response, and a call belonging to another one throws
`HttpServerRuntimeException`.

| Mode | Calls | Reaches the client | Framing |
|---|---|---|---|
| Buffered | `setBody()`, `appendBody()`, `json()`, `html()` | at `end()` | `Content-Length`, computed from the buffer |
| Streamed | `write()`, `sseEvent()`, `writeMessage()` | at each call | chunked encoding (HTTP/1) or DATA frames closed by END_STREAM (HTTP/2, HTTP/3); a `Content-Length` set before the first `write()` frames the body instead — see below |
| File | `sendFile()` | after the handler returns | `Content-Length` from the file; a satisfiable `Range` yields `206` with `Content-Range` |

### Buffered

```php
$res->setBody('one ')       // replaces the buffer
    ->appendBody('two')     // appends to it
    ->setHeader('Content-Type', 'text/plain')
    ->end();                // 7 bytes leave here, Content-Length: 7
```

Nothing is committed until `end()`, so status and headers stay writable for as
long as the handler runs, and `getBody()` returns what has accumulated. This is
the mode to use when the body fits in memory: one write syscall, and the
compressor sees the whole payload at once.

### Streamed

```php
$res->setStatusCode(200)->setHeader('Content-Type', 'text/plain');
foreach ($rows as $row) {
    $res->write(format($row));   // the first call commits status + headers
}
$res->end();
```

The first `write()` commits the status line and the headers; from then on
`setStatusCode()`, `setHeader()` and `setBody()` throw, and `isHeadersSent()`
answers true.

#### Framing by declared length

A `Content-Length` set before the first `write()` reaches the client on every
protocol, and the body is framed by it: HTTP/1 sends no `Transfer-Encoding`, no
chunk size lines and no terminator, and HTTP/2 and HTTP/3 carry the field
alongside the DATA frames so the peer can check one against the other.

The server then holds the body to that number.

```php
$res->setHeader('Content-Length', (string) $size);
$res->write($first);        // counted against $size
$res->write($rest);         // throws if the two together pass it
$res->end();                // short of $size: the stream is failed, not finished
```

A write that would pass the declared length throws `HttpServerRuntimeException`
and queues nothing, so the handler can catch it and still finish the body it
promised. A body that ends short is failed the way `abort()` fails one —
HTTP/1 withholds the last bytes and drops keep-alive, HTTP/2 and HTTP/3 reset
the stream — because ending it cleanly would tell the client it has a whole
body when it has part of one.

Two consequences follow from holding the body to a number. The response is not
compressed: a codec would put a different count on the wire, which is why it
deletes the header when it engages. And a graceful shutdown that interrupts
such a stream fails it rather than ending it cleanly, unlike an undeclared
stream, whose bytes were never promised in advance.

A `Content-Length` that is not a decimal byte count, or one set twice through
`addHeader()`, throws from that first `write()` — while the response is still
uncommitted, so the handler can still answer with a status. Only `write()` and
`tryWrite()` declare: `sseEvent()` and `writeMessage()` frame their own
records, a `HEAD` response keeps the length on the buffered path where it
describes the body a `GET` would return, and a status that carries no content
(`1xx`, `204`, `304`) declares nothing.

`write()` parks the handler coroutine while the outbound queue is full: HTTP/2
and HTTP/3 park once every ring slot is live or the queued bytes reach
`HttpServerConfig::setStreamWriteBufferBytes()` (256 KiB by default), HTTP/1
parks on the socket write itself. Three calls cover the cases where parking is
the wrong answer:

- `tryWrite($chunk)` queues the chunk or answers false, having queued nothing —
  the same chunk can be offered again. HTTP/1 keeps no queue of its own, so it
  never refuses and an accepted chunk still waits for the socket.
- `awaitWritable($timeoutMs = null)` waits for the room `tryWrite()` refused.
  It answers false where a transport can be full and offers no wait, since
  "go ahead" would spin a handler that trusts it.
- `isWritable()` reports whether output is still possible at all: `end()` not
  called, no `sendFile()` seal, peer still there. A false answer is final,
  which is what makes it the right condition for leaving a streaming loop.

The two dialects have the same pair. `trySseEvent()` and `tryWriteMessage()`
answer false on a full queue with nothing queued and no header committed, so
the same event or message can be offered again after `awaitWritable()` — which
an SSE handler may call, because waiting puts no bytes of its own on the wire.
Where `awaitWritable()` answers false it did not wait, so the retry belongs
after a fall back to the blocking twin rather than inside a loop around it;
HTTP/3 is where that matters, since it refuses once per chunk on a congested
path and offers no wait to park on.

A peer that departs mid-stream arrives as `HttpException` with code 499 out of
the next call, so a `try`/`catch` around the loop is how a handler winds down.
`isEnded()` reports the response, not the connection: it stays false until the
handler finishes it, with `end()` or with `abort()`.

### A stream that failed

`end()` tells the client the body is whole, and once the status is on the wire
there is no status left to say otherwise. `abort()` is the other ending:

```php
try {
    foreach ($rows as $row) {
        $res->write(format($row));
    }
    $res->end();
} catch (\Throwable $e) {
    $res->abort();      // the client must not read this as a complete export
    throw $e;
}
```

What it costs is protocol-specific, and that is the point — each protocol has
exactly one way to report a body that stopped:

| | On the wire | What the client sees |
|---|---|---|
| HTTP/1.1 | no terminating chunk, connection closed | `curl` exits 18, `CURLE_PARTIAL_FILE` |
| HTTP/2 | `RST_STREAM(INTERNAL_ERROR)` | a stream error; other streams on the connection are unaffected |
| HTTP/3 | `RESET_STREAM(H3_INTERNAL_ERROR)` | the same, per stream |

`abort($errorCode)` names the code instead. It is the reset code of whichever
protocol is carrying the response and does not travel between them — HTTP/2 and
HTTP/3 number the same conditions differently, and HTTP/1 has no field for one,
so a handler that names a code should know its protocol from
`getProtocolVersion()`. The range is 0 to 4294967295; anything else throws.

A handler that throws without catching gets this anyway: the server aborts a
committed stream on its own rather than finishing it. A cancellation is not a
failure and still ends cleanly, so a graceful shutdown does not truncate the
feeds it interrupts. A stream that declared a `Content-Length` is the exception:
there the promise is a byte count, so a body short of it is failed whatever
stopped the handler.

How much of the partial body the client keeps is the transport's business, not
a promise: HTTP/1 has already written everything the handler handed over,
HTTP/2 discards whatever it had queued but not yet framed, and HTTP/3 flushes
what it holds before resetting. A client must discard the body of an aborted
response either way, which is the point.

There is a body to disown only in the third of these states, and only the
fourth is an error:

| State when `abort()` is called | What happens |
|---|---|
| never started streaming — buffered, or a `HEAD` | nothing; the handler's own exception still becomes the status |
| started, nothing on the wire yet — `sseStart()` with no event | finished cleanly: the client gets the empty `200` the transport commits for it |
| started, and bytes have reached the client | the stream is failed as above |
| `end()` already called | throws — the client has been told the body is whole |

A second `abort()` is a no-op, because its place is a catch block where a throw
would bury the handler's own error.

In the second state a declared `Content-Length` is corrected rather than
carried: nothing has left, so the empty response the transport commits states
the count actually written instead of leaving the client waiting for bytes that
are not coming.

Under compression the codec stream is left unclosed too. A gzip trailer is a
second claim that the body is whole, and a decoder checks it.

### File

```php
$res->sendFile('/srv/assets/app.js');
```

`sendFile()` seals the response and returns at once; the file is delivered
after the handler returns. Every mutating call afterwards throws, including a
second `sendFile()`. Options — cache headers, download disposition, precomputed
compressed variants — go in a `SendFileOptions` passed as the second argument.

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

### Metrics — `getStats()`

Opt in with `setStatsEnabled(true)`; without it `getStats()` throws (the
per-worker counter slab is only allocated when stats are enabled). The hot-path
counter bumps themselves are always on and cheap.

```php
$config->setStatsEnabled(true);
// … once running, from any thread:
$stats = $server->getStats();
```

The shape is **nested**, not flat — per-worker slots plus a summed total:

```php
[
    'enabled' => true,
    'workers' => [ 0 => ['total_requests' => …, 'conns_active_h1' => …, …], 1 => […] ],
    'totals'  => ['total_requests' => …, 'responses_2xx_total' => …, …],  // summed
]
```

`*_total` fields are monotonic counters (and survive a pool reload);
`conns_active_*`, `active_requests`, `h2_streams_active` are point-in-time
gauges. Build any exposition format (Prometheus, OTLP, StatsD) in PHP on top of
this array — the server ships no embedded exporter. See
`examples/observability-server.php` for a Prometheus `/metrics` endpoint.

### Logging — sinks and formatters

`setLogSeverity()` + `setLogStream()` is the single-stream sugar. For more,
`setLogSinks()` fans each record out to up to 8 sinks, each with its own
severity floor, formatter and category:

```php
use TrueAsync\LogSeverity;

$config->setLogSinks([
    // structured access log (one record per completed request) as JSON to a file
    ['type' => 'file', 'path' => '/var/log/access.log', 'format' => 'json',
     'category' => 'access', 'level' => LogSeverity::INFO],
    // human diagnostics to a coloured console
    ['type' => 'stderr', 'format' => 'pretty',
     'category' => 'app', 'level' => LogSeverity::WARN],
]);
```

`type` is `stream` / `file` / `stdout` / `stderr` / `syslog`; `format` is
`plain` / `logfmt` / `json` / `pretty` / `template`; `category` routes record
kinds (`app` diagnostics, `access` per-request, `all`). Under a worker pool use
`file` (each worker reopens the path) — a parent-opened `stream` resource cannot
cross into worker threads. No sink calls back into PHP: records are emitted from
IO and reactor threads with no VM context, so export from userland by draining a
`json` file/socket sink in your own coroutine.

### Trace context

```php
$config->setTelemetryEnabled(true);   // ingest W3C traceparent / tracestate
```

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

### Shutting the pool down

`$server->stop()` works on the pool parent. It retires every worker over
the pool's control channel and suspends until they have drained and the
listen sockets are closed, so it returns only once the server is really
down. Call it from a coroutine — typically a signal handler:

```php
Async\spawn(function () use ($server) {
    Async\signal(SIGTERM);
    $server->stop();               // returns when the pool is down
});

$server->start();                  // returns at the same moment
```

### Caveats

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
