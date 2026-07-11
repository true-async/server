# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- **Observability — telemetry metrics + logging redesign (#5).** Metrics are
  read through a plain PHP array (no embedded exporters); logs fan out to
  pluggable sinks.
  - **Cross-worker stats (`HttpServer::getStats()`).** Opt-in via
    `HttpServerConfig::setStatsEnabled(true)`. Each pool worker owns one slot in
    a process-wide, cache-line-aligned counter slab and bumps it lock-free (no
    atomics on the hot path); `getStats()` walks the slab from any thread and
    returns `{enabled, workers, totals}`. Totals include `total_requests`,
    per-status-class `responses_2xx/3xx/4xx/5xx_total` (each request classified
    exactly once, so the four sum to `total_requests`), and per-protocol active
    gauges `conns_active_h1/h2/h3`. Throws when stats are disabled.
  - **Multi-sink logging (`HttpServerConfig::setLogSinks()`).** A log record now
    fans out to several sinks at once, each with its own severity floor and
    formatter; the fast gate is the minimum floor across sinks, and one failing
    sink (drop-counted) never blocks the others. Emit formats once per distinct
    formatter before fan-out. `setLogSinks([['type'=>'stream'|'stdout'|'stderr',
    'stream'=>$res, 'format'=>'plain'|'logfmt'|'json'|'pretty',
    'level'=>LogSeverity::…], …])` declares up to 8 sinks (invalid specs throw at
    config time); `setLogSeverity()`/`setLogStream()` stay as single-stream sugar.
    Each sink writes through the stream's own async IO handle and batches records
    in a per-sink ring buffer flushed at a 32 KiB high-water mark or a 200 ms
    timer, so a burst of logs coalesces into far fewer write syscalls while the
    emit call itself never blocks.
  - **syslog sink — TCP, UDP and unix datagram.** `['type'=>'syslog',
    'target'=>'tcp://host:port' | 'udp://host:port' | 'udg:///dev/log',
    'facility'=>'local0', 'level'=>…]` ships records as RFC 5424 messages. The
    formatter emits the bare message; framing belongs to the transport: TCP
    gets RFC 6587 octet-counted framing (a receiver splits records even when a
    message carries an embedded newline), while UDP and unix-datagram targets
    send exactly one record per datagram so message boundaries survive. PRI
    packs the configured facility with the severity mapped to syslog levels.
  - **Structured access log (`'category' => 'access'`).** A sink spec may
    carry `'category' => 'app' | 'access' | 'all'` (default `'app'`): `app`
    receives server diagnostics, `access` receives exactly one structured
    record per completed request — `method`, `path`, `status`, `proto`
    (h1/h2/h3), `bytes` (response body), `duration_ms`, `remote` (`ip:port`)
    and the W3C trace context when telemetry parsed one — so a JSON access
    log and a pretty diagnostics console coexist on one server. Emitted on
    every completion path (handler return, static handler, compression
    reject, sendFile engine, reactor-pool worker dispatch) across HTTP/1,
    HTTP/2 and HTTP/3. Off by default: without an access sink the cost is
    one predicted branch per request; with one, the peer address is resolved
    once per connection and timestamp rendering caches the current second.
  - **Sink-type / formatter registry (plugin seam).** `setLogSinks()` resolves
    `'type'` and `'format'` names through a registry instead of hardcoded
    lists; another extension can add its own sink type or formatter at MINIT
    via `http_log_register_sink_type()` / `http_log_register_formatter()`
    (built-ins register through the same seam). Validation error messages list
    whatever is actually registered. The `syslog` formatter is now also
    name-addressable (`'format'=>'syslog'`) on stream sinks.
  - **`template` formatter — user-controlled line layout.** `['format' =>
    'template', 'template' => '{ts:Y-m-d H:i:s.v} [{level}] {msg}{attrs}']`
    renders each record through a custom template: `{ts}` (ISO-8601) or
    `{ts:PATTERN}` with a PHP `date()`-style subset (`Y y m d H i s v`),
    `{level}`, `{msg}`, `{attrs}`, `{trace}`, `{span}`; everything else is
    literal (unknown placeholders pass through verbatim). The template is
    compiled once when the sink starts, so the per-record render stays a flat
    segment walk. Bad templates throw at `setLogSinks()` time.
  - **Formatters: `plain`, `logfmt`, `json`, `pretty`.** `json` is one
    OTel-Logs object per line (Timestamp/SeverityNumber/SeverityText/Body/
    Attributes/TraceId/SpanId, RFC 8259 escaping); `logfmt` is `key=value` with
    quoting; `pretty` is a coloured console line
    (`HH:MM:SS.mmm  LEVEL  message  key=val …`) whose colour is decided once at
    sink build from the target fd, honouring `NO_COLOR` / `CLICOLOR_FORCE`.

- **gRPC over HTTP/2 and HTTP/3 (#4).** Requests whose content-type begins with
  `application/grpc` route to the callable registered via
  `HttpServer::addGrpcHandler()`; everything else is untouched, so gRPC and
  regular HTTP handlers coexist on one listener.
  - **All four RPC shapes** — unary, server-streaming, client-streaming and
    true full-duplex bidi: `HttpRequest::readMessage()` deframes the request
    stream incrementally (5-byte length-prefix framing, 16 MiB per-message cap),
    `HttpResponse::writeMessage()` frames replies; the handler starts on
    HEADERS, before the body finishes.
  - **Trailers**: `grpc-status`/`grpc-message` ride real HTTP trailers on both
    transports (nghttp2 trailer HEADERS; `nghttp3_conn_submit_trailers` at true
    EOF on H3 — verified with a real aioquic client). `grpc-status: 0` is
    defaulted on success, `13 INTERNAL` on an uncaught handler exception, and a
    handler that writes no messages gets the canonical Trailers-Only reply.
  - **grpc-web (binary)**: `application/grpc-web` calls carry their trailers
    in-body as a `0x80`-flagged frame, on H2 and H3.
  - **gzip message encoding**: inbound `grpc-encoding: gzip` messages inflate
    transparently in `readMessage()`; `setGrpcEncoding('gzip')` declared
    before the first `writeMessage()` compresses every reply frame (per-call
    declaration, mirroring grpc-go/java/C++ — a compressed frame without a
    declared encoding cannot be expressed).
  - **`grpc-timeout`** request header parsed and exposed via
    `HttpRequest::getGrpcTimeout()`.
  - **grpc-web-text**: `application/grpc-web-text` calls carry base64 both
    directions — `readMessage()` decodes the request transparently, every
    response frame (messages + the trailer frame) goes out independently
    base64-encoded.
  - **Works under the reactor pool** (`TRUE_ASYNC_SERVER_REACTOR_POOL=1`) —
    gRPC rides the generic streaming reverse path below; no gRPC-specific
    code in the reactor/worker split.

- **Reactor-pool streaming reverse path (#80).** Under
  `TRUE_ASYNC_SERVER_REACTOR_POOL=1` a worker response is no longer
  buffered-only:
  - `send()`/`writeMessage()`/SSE stream across the thread boundary — the
    worker posts STREAM_HEADERS / STREAM_CHUNK / STREAM_END wires in FIFO
    order; the reactor feeds its existing chunk ring and submits native
    trailers at true EOF (so `setTrailer()` works under the pool, buffered
    or streamed).
  - **Credit-based backpressure**: a per-stream credit block (atomics,
    malloc-domain) paces the producer — over 1 MiB un-acked in flight the
    handler coroutine parks and resumes as the QUIC peer acknowledges
    bytes, so a slow client cannot flood the shared reactor mailbox. Peer
    RST / connection close unparks it into the standard stream-dead path
    (`send()` throws 499).

- **HTTP/3 streaming request bodies (#26 policy on H3).** With
  `setBodyStreamingEnabled(true)` the H3 dispatch now applies the same
  three-case Content-Length policy as HTTP/2, so `readBody()` and
  incremental `readMessage()` (true full-duplex gRPC) work over HTTP/3.
  QUIC flow-control credit is deferred: the window refills as the handler
  drains chunks, bounding un-read bytes by `max_body_size`.

### Fixed

- **HTTP/3 uploads larger than the initial stream window (256 KiB default)
  stalled forever.** `nghttp3_conn_read_stream`'s consumed count excludes
  DATA payload by contract, and nothing extended the QUIC windows for
  buffered body bytes — now `h3_recv_data_cb` returns the credit as it
  consumes them.

- **Three latent use-after-frees found under ASAN** (masked by the Zend
  arena in normal runs): the WebSocket dispose read `w->committed` after
  the zval dtor could free `w`; `http_log_server_stop` awaited a write
  request that its own completion callback frees (now drains by yielding
  to the reactor and re-polling); the WebSocket reject / spawn-fail paths
  freed a request the H1 parser still borrowed via `parser->request`.

### Performance

- **HTTP/3 reactor-pool hot paths** (reactor review follow-up): reactor
  commands travel the mailbox by value (no malloc/free per message),
  O(1) intrusive stream unlink, listener local sockaddr cached per peer
  family, thread-local cipher context in CID steering, and a per-worker
  memory budget for H3 static delivery (ported from H2). Hard
  backpressure on a full worker inbox now RESETs the stream with
  `H3_REQUEST_REJECTED` instead of silently dropping the request.

### Changed

- **gRPC layering: call-lifecycle policy extracted out of the transports (#4).**
  `src/grpc/grpc_call.c` owns response defaults, outcome → `grpc-status` and
  delivery shape (grpc-web in-body frame / streaming EOF / Trailers-Only);
  HTTP/2 and HTTP/3 provide a 3-op wire vtable and stay gRPC-agnostic on
  delivery. H3 response-trailer capture/submit is now generic — any streaming
  response with a trailer map is delivered, not just gRPC (parity with H2).

## [0.9.3] - 2026-07-07

### Fixed
- **Clean pool shutdown no longer leaks or crashes (#93).** On `Async\graceful_shutdown()` the pool parent now disposes the per-worker completion futures it owns from `submit_internal()`, so their cross-thread wakeup triggers no longer linger armed on the parent reactor (loop-alive assert on debug / leaked libuv handle on release). Also fixes a use-after-free from double-disposing the parent's `all_done` wait event (the waker already disposes it) — a hard crash on macOS ARM64 release. Requires php-async ≥ 0.7.9 for the race-safe `remote_future_dispose`. New tests: `055`–`057`.

### CI
- Disable `opcache.protect_memory` in the phpt suite — its process-global `mprotect` races across the threaded worker pool (false SIGBUS); it defaults off in production and real compilation is serialized by the SHM lock.
- Collect and upload symbolicated crash backtraces on test failure (macOS DiagnosticReports).

## [0.9.2] - 2026-07-03

### Added

- **`HttpServer::reload()` — hot reload of the worker pool without dropping the
  listen sockets (#93).** Pool-parent only. Bumps a shared epoch beacon
  (`pemalloc`'d, parent-owned, fanned out to worker clones through the transit
  shells); workers watch it from the deadline tick and retire themselves from a
  fresh main-scope coroutine (drain in-flight → `stop` → exit to the closed pool
  channel). The pool then rotates via the ThreadPool ABI `reload()` — replacement
  threads re-run the bootloader, so changed code is picked up — and one `start()`
  task per worker is resubmitted onto the fresh channel. Suspends until the old
  cohort has fully drained. Reload is serialized (one rotation at a time) and the
  lifecycle is logged (`reload.start` / `reload.done`, per-worker
  `server.stop reason=reload`).
- **Built-in hot-reload triggers (#93).**
  - `HttpServerConfig::enableHotReload(array $watchPaths, array $extensions = ['php'], int $debounceMs = 300, int $maxHoldMs = 2000)` —
    dev trigger: the pool parent spawns one recursive `Async\FileSystemWatcher`
    per path; a debounced change event invalidates the watched trees in opcache
    and calls `HttpServer::reload()`.
  - `HttpServerConfig::enableReloadOnSignal(bool $enabled = true)` — prod trigger:
    the pool parent arms a persistent `SIGHUP` handler that calls
    `HttpServer::reload()`.

- **Reload under the reactor pool (#93).** `HttpServer::reload()` now works with
  `TRUE_ASYNC_SERVER_REACTOR_POOL=1`. A worker-inbox retirement protocol
  unpublishes the retiring slot (admin mutex; picks stay lock-free), fences every
  reactor so no pre-retire inbox pointer survives, and a zero-crossing decrement
  wakes the worker to free its inbox — connections homed to the slot re-home on
  their next request. Slots are reclaimed so a rotated pool can re-register.

### Fixed

- **~10 KB leaked per reload rotation (#93).** The worker transit shell's C-state
  and side-cars were not released when the old cohort exited; now freed after the
  rotation completes.
- **Heap corruption on rotation under the gated reactor pool (#93).** A dying
  worker clone's free path unconditionally tore down the *global* worker registry
  and `g_reactor_pool` from the worker thread while the parent's reactors were
  live; the catch-all teardown is now parent-only.
- **macOS build: TSRM mutex instead of `uv_mutex` in the worker registry (#93)** —
  macOS TUs have no libuv include path.

### Changed

- **Test suite: kernel-allocated ports across every phpt (#93).** All phpt now bind
  to an OS-assigned port instead of a fixed one, eliminating the port-collision
  flake class (previously seen on the `h2/012` + `core/051`/`052` cluster).
- **Ctrl+C signal-delivery test harness (#94)** for macOS/Linux, covering
  interrupt, `SIGTERM`, `pcntl`-before/after, open-connection, multi-waiter, and
  dev-server scenarios.

## [0.9.1] - 2026-07-02

### Fixed

- **Static-build header discovery.** Canonical flat `php_true_async_server.h`
  registration header so `genif`/static builds resolve the extension header; added
  a flat `php_server.h` shim and renamed `http_server_module_entry` →
  `true_async_server_module_entry`. WebSocket server 0.9.0 feature set unchanged.
- **`htons`/`ntohs` declared for the bundled wslay under strict C99 compilers**
  (clang / zig-cc), fixing the WebSocket build on those toolchains.

## [0.9.0] - 2026-07-01

### Added

- **WebSocket support (RFC 6455) (#2).** Full-duplex over HTTP/1.1
  Upgrade, `wss://`, and HTTP/2 Extended CONNECT (RFC 8441), with permessage-deflate
  (RFC 7692). `HttpServer::addWebSocketHandler()`; `WebSocket` / `WebSocketMessage` /
  `WebSocketUpgrade` classes, `WebSocketCloseCode` enum, exception hierarchy.
- **Pull API** — `recv()` and `foreach ($ws as $msg)` (`WebSocket` is an `Iterator`);
  a graceful close ends the loop, an error close throws `WebSocketClosedException`
  carrying `$closeCode` / `$closeReason`.
- **Multi-producer send** — `send()` / `sendBinary()` safe from any coroutine, plus
  non-blocking `trySend()` / `trySendBinary()` and `WebSocketBackpressureException`
  under sustained backpressure.
- **Keepalive** — server-initiated ping (`ws_ping_interval_ms`) and a pong deadline
  (`ws_pong_timeout_ms`) that closes an unresponsive peer with 1001.
- **Outbound auto-fragmentation** — messages larger than `ws_max_frame_size` are
  split into continuation fragments no larger than the cap.
- **Conformance & fuzzing** — Autobahn|Testsuite runner (`e2e/autobahn/`, built from
  source in Docker, 246/246 on `behavior`) wired into CI, plus a wslay frame-ingress
  libFuzzer harness (`fuzz/fuzz_ws_frame.c`).

### Fixed

- **UTF-8 fail-fast no longer lingers the socket (#2).** On a protocol error wslay
  queues a CLOSE but `wslay_event_recv` still returns 0, so the handler stayed parked
  in `recv()` and the TCP hung forever once the peer echoed the close; now detected
  via `wslay_event_want_read()` and torn down.
- **Handshake-reject paths no longer leak the parsed request (#2).**

### Performance

- **One write per WebSocket frame (#2).** Frame header and payload were written
  separately (two `write()` syscalls per frame); coalesced into one — 51% fewer
  write syscalls and ~43% higher echo throughput under load.

## [0.8.1] - 2026-06-28

### Fixed

- **SSE/streaming: a client that aborts mid-stream no longer crashes the server (#3).**
  When the peer sent a RST, the next write's `uv_write()` failed at *submit* and the
  reactor left an `Async\AsyncException` ("Failed to start stream write: broken pipe")
  in `EG(exception)`. The awaiting send path (`http_connection_send_raw`) returned
  failure without absorbing it — unlike a *completion* failure, which
  `async_io_req_await()` already clears, and unlike the fire-and-forget writers, which
  call `http_absorb_io_submission_exception()`. The orphaned exception then surfaced
  with no PHP frame (`#0 {main}`) as an uncaught fatal, taking down every connection.
  The submit-failure branch now absorbs it too, so a dead peer reaches the handler as
  the canonical, catchable `HttpException` (499 "stream closed by peer"). New phpt
  `025-h1-sse-client-disconnect` reproduces the crash (RST mid-SSE) and asserts the
  499 instead.
- **H3 static-file pump now absorbs a read-submit failure too (#3).** The same
  asymmetry on the file-read side: when `ZEND_ASYNC_IO_READ` failed at submit, the
  producer coroutine broke out of the pump loop without clearing the reactor
  exception it left in `EG(exception)`, which would then surface as an uncaught
  fatal on unwind. It now absorbs it (the completion-error case was already
  handled via `req->exception`), keeping error handling symmetric across the
  write and read submit paths.

## [0.8.0] - 2026-06-27

### Added

- **Server-Sent Events API (#3).** First-class `text/event-stream` helpers on
  `HttpResponse` — `sseStart()`, `sseEvent($data, $event, $id, $retry)`,
  `sseComment()` and `sseRetry()` — layered on the existing streaming pipeline,
  so the same handler works over HTTP/1.1, HTTP/2 and HTTP/3. `sseStart()` sets
  the canonical headers (`Content-Type: text/event-stream`, `Cache-Control:
  no-cache, no-transform`, `X-Accel-Buffering: no`) and marks the response
  non-compressible. Framing follows WHATWG §9.2: multiline `data` is split per
  line, single-line fields reject CR/LF and `id` rejects NUL. phpt coverage for
  H1/H2/H3 plus the validation surface.

- **hq-interop (HTTP/0.9-over-QUIC) for the interop matrix (#80).** A second QUIC
  ALPN, `hq-interop`, served straight off the transport (no nghttp3): a raw bidi
  stream `GET <path>` returns the file bytes + FIN from `setHttp3HqDocroot()`.
  Lets the quic-interop-runner reach the server for the whole transport matrix
  (transfer/multiplexing/migration/loss), which it negotiates over hq, not h3.
  `h3` stays preferred when a peer offers both; the h3 path is unchanged.

- **HTTP/3 transport reactor pool (experimental, #80).** Behind
  `TRUE_ASYNC_SERVER_REACTOR_POOL=1` + `setWorkers(2+)`: dedicated C reactors own the
  QUIC sockets (no PHP on the transport thread), hand parsed requests to PHP workers
  by pointer, and serve responses back over a non-blocking reverse channel; static
  files are served on the reactor. Adds CID steering (owner-reactor id encoded in the
  connection id, forwarding migrated clients across the split — #72) and a
  migration-storm guard that sheds clients rebinding past a rate cap. Dispatch is
  reactor-paired: a connection sticks to one of its reactor's workers and spills to a
  less-loaded worker when its home backs up or dies. Off by default.
- Lock-free inter-thread message queue primitive (#81): bounded MPSC/SPSC C-ABI
  wrappers over moodycamel (`thread_queue`) plus a reactor-integrated MPSC mailbox
  (`thread_mailbox`) that wakes the consumer's loop via a trigger event with
  lost-wakeup-safe batch drain. Foundation for cross-worker HTTP/3 (#72) and
  WebSocket (#2). Adds a C++ build dependency (libstdc++).

### Fixed

- **SSE: `sseStart()` with no event now commits an empty `200` on H2/H3 (#3).**
  Starting an event stream and closing it before any `sseEvent()`/`sseComment()`
  left HTTP/2 and HTTP/3 without a HEADERS frame (the client saw a reset stream),
  while HTTP/1.1 already sent a clean empty `text/event-stream`. `mark_ended` now
  commits the empty streaming response on all three protocols.
- **SSE: mixing `send()` and the `sse*` helpers now throws (#3).** A response is
  either a plain `send()` stream or an SSE stream; crossing over silently shipped
  wrong-`Content-Type` (and possibly gzip-wrapped) event records. Each side now
  raises `HttpServerRuntimeException` once the other has committed the stream.
- **Windows: TCP listeners now bind.** The server failed to start on Windows
  with `Async\AsyncException: Failed to bind to <host>:<port>: operation not
  supported on socket`. The listener requested `SO_REUSEPORT`, which libuv's
  `uv_tcp_bind()` rejects with `UV_ENOTSUP` on Windows (Winsock has no
  `SO_REUSEPORT`). REUSEPORT is now treated as a platform capability and never
  requested on Windows; the default single-listener server binds directly. No
  change on Linux/BSD/macOS (#82).
- **Windows: `StaticHandler` accepts native absolute paths.** Root-directory
  validation only accepted a leading `/`, rejecting every Windows path
  (`C:\...`) and making `StaticHandler` unusable there. It now uses
  `IS_ABSOLUTE_PATH` (drive-letter / UNC on Windows, leading `/` on POSIX).
- **Windows: static file bodies are served binary-clean.** The `send_file`
  engine opened files without `O_BINARY`, so Windows text-mode translation
  could corrupt or truncate binary bodies (precompressed `.br`/`.gz`, byte
  ranges, images). It now opens with `O_BINARY`, matching the policy open path.

## [0.7.2] - 2026-06-02

### Added

- `HttpServerConfig::setRequestScope(bool)` / `isRequestScope()` — opt out of the
  per-request child async scope (default on). Off reuses the connection scope,
  saving two allocations per request; `Async\request_context()` then returns null
  (use `?->`). Propagates across `setWorkers(N>1)`.

## [0.7.1] - 2026-06-01

### Fixed

- HTTP/3: replenish the connection's bidi stream credit on stream close
  (`extend_max_streams_bidi`), so long-lived connections no longer stall at the
  `initial_max_streams_bidi` cap (#79).

## [0.7.0] - 2026-06-01

Headline release: **HTTP/3 over QUIC**. Folds in everything tagged but not yet
documented since 0.6.7 (the 0.6.8 tag carried no changelog entry).

### Added

- **HTTP/3 / QUIC server** (`HttpServerConfig::addHttp3Listener`) — full request
  lifecycle over QUIC: end-to-end GET/POST with `awaitBody`, streaming `send()`,
  HEAD, `sendFile()` delivery, and `addStaticHandler` mount routing. Built on
  ngtcp2 + nghttp3 + OpenSSL ≥ 3.5; auto-detected (`--enable-http3` /
  `--disable-http3`).
- HTTP/3 production controls: connection migration / NAT rebinding (RFC 9000 §9),
  opt-in send pacing (`setHttp3Pacing`), per-peer connection budget with global
  cap and explicit refusal, configurable UDP socket buffer
  (`setHttp3SocketBufferBytes`), idle timeout, Alt-Svc advertisement, Retry token
  source-address validation, version negotiation, and stateless reset.
- `HttpServer::getHttp3Stats()` — handshake / ALPN / nghttp3 / send-error counters.
- `HttpServer::isHttp2()` / `isHttp3()` compile-time capability probes.
- `HttpServerConfig::setTlsBufferBytes` — tunable TLS clear-text-out BIO ring (#29).
- Shared-fd TCP listener path for workers on kernels without load-balancing
  `SO_REUSEPORT`, selectable at runtime.

### Changed

- HTTP/3 send path coalesces outbound datagrams to once-per-tick and splits
  coalesced inbound datagrams via `UDP_GRO`; UDP socket buffers enlarged.
- HTTP/1 conformance hardening: `Date` header, HEAD sends no body, reject
  `CONNECT` and asterisk-form targets, validate `Host`, reject empty
  `Transfer-Encoding`, reject fragment/backslash in request-target, reject
  duplicate `Content-Type`.
- HTTP/2 over TLS parks the emit remainder when the clear-text-out BIO ring fills
  (backpressure instead of a write deadlock) (#29).
- `HttpServer::start()` now throws on listener bind failure instead of failing
  silently.

### Fixed

- Drain in-flight per-request coroutines on server shutdown so `server_scope` is
  not disposed while handlers are still running (#74).
- HTTP/3: dirty-list use-after-free on connection free, dispatched-stream slot
  leak when a stream is rejected mid-`awaitBody`, and `arm_timer` NULL-`ngtcp2_conn`
  guard.
- `http_server`: use-after-free of the wait event on non-stop teardown.
- Windows MSVC build.

## [0.6.7] - 2026-05-27

### Fixed

- Windows build (`config.w32`): add missing sources `src/http_response_server_api.c` and `src/http1/http1_format.c` so the MSVC build links after the `http_response.c` split landed in 0.6.6.

## [0.6.6] - 2026-05-27

### Changed

- Code audit (issue #37) — Phases 1–6 done. `src/http_response.c` (2173 lines) split into PHP-class TU + `src/http1/http1_format.c` (H/1 wire formatters) + `src/http_response_server_api.c` (server-side C-API for static/h2/compression paths). No behaviour change; phpt 211/211 PASS.

### Added

- `HttpServer::getRuntimeStats()` — snapshot of `conn_arena` (live slots, committed chunks, byte commitment) and `body_pool` (per-class LIFO of large request bodies). Pairs with `Async\runtime_stats()` and `zend_mm_dump_live_allocations()` to attribute RSS growth to a concrete subsystem.

### Fixed

- `034-config-tls-and-log.phpt`: drop the `curl_close()` call that emits a Deprecated notice on PHP 8.5+ (no-op since 8.0).
- License headers added to compression / http3 / core files that were missing them.

## [0.6.5] - 2026-05-20

### Added

- Per-request scope: each request handler coroutine now runs in its own scope, a child of the server scope, so `Async\request_context()` resolves to a context shared across the whole request coroutine subtree while `Async\current_context()` stays per-coroutine.
- IDE stubs (`ide-stubs/true-async-server.php`) for editor autocompletion of the `TrueAsync\*` API.

### Fixed

- `stubs/HttpRequest.php` was missing the `readBody()` declaration although the method ships in the extension — the stub now matches the generated arginfo.

## [0.6.4] - 2026-05-20

### Fixed

- HTTP/1 pipelining crash under high connection count (HttpArena `pipelined/4096c`): a handler-coroutine spawn failure destroyed the connection — freeing its llhttp parser — synchronously from inside `llhttp_execute` (dispatch fires from `on_headers_complete`), causing a use-after-free SIGSEGV in `on_message_complete`. Connection teardown now defers (`in_parser_feed` guard) while a parser feed is on the stack and is finalised once the feed unwinds.
- Fire-and-forget I/O write submit failures (broken pipe / connection reset) left an `Async\AsyncException` stranded in `EG(exception)` with no coroutine to receive it; it then aborted an unrelated `ZEND_ASYNC_NEW_COROUTINE` — the spawn failure above. The batched-send paths now log and clear the exception at the submission site.

## [0.6.3] - 2026-05-19

### Added

- One-shot brotli compress path with `BROTLI_PARAM_SIZE_HINT` (Step 4 of perf TODO): `apply_buffered` uses the stateless one-pass `BrotliEncoderCompress()` when the body is fully known. The size hint lets the encoder right-size its ring buffer / hash tables for the actual payload instead of for arbitrary streaming. New optional vtable slots `compress_oneshot` + `max_compressed_size`; streaming path stays for chunked / unknown-length responses. Closes the brotli encode gap vs Swoole's `BrotliEncoderCompress`-based path. C-side defaults stay production-typical (gzip 6, brotli 4); bench callers set `setCompressionLevel(1)` / `setBrotliLevel(1)` for Swoole-equivalent throughput.
- Loud stderr logging on unexpected worker thread exits in `pool_worker_handler` — covers uncaught `$server->start()` exceptions, clean returns while the await loop still expects workers, and server-transfer failure. Previously each case silently dropped 1/N of accept capacity with no operator signal.

### Fixed

- `Connection: close` request header now produces `Connection: close` in the response too (RFC 9112 §9.6). The parser already flipped `req->keep_alive = false` and the dispose path closed the FD, but the missing response header left clients unable to tell the TCP was not reusable until the next write hit ECONNRESET — wrk under `-H 'Connection: close'` counted every reply as a read error. Side effect on the local short-lived bench (wrk c=512 d=10s): 174k → 230k RPS, p50 14.5 ms → 2.5 ms, read-errors 2.0M → 0.

### Changed

- Server-side codec preference order flipped to `zstd > gzip > brotli > identity`. Clients sending the common `gzip, br` Accept-Encoding now get gzip — the brotli pool can't reuse encoder state (libbrotli has no public reset API), so until the arena-allocator follow-up (TODO Step 4) lands, gzip's `deflateReset` path is the better default. Clients that explicitly want brotli via q-values (`br;q=1.0, gzip;q=0.5`) still get it.

## [0.6.2] - 2026-05-19

### Added

- HTTP/2 over TLS hybrid emit selector (#30): small responses take the DRAIN path (mem_send + BIO_write, no gather alloc churn); bodies > 2 KiB or streaming take GATHER (NO_COPY refs + one SSL_write_ex). Streams pin a per-session counter at submit time. Bench (release PHP, c=100 m=32, h2load -t 1): dyn 3B 243k / 16K 57k / 64K 18k — hybrid best-of-three across the matrix. Env override `TRUE_ASYNC_H2_TLS_EMIT_MODE = drain | gather | hybrid` for A/B.
- `docs/H2_TLS_EMIT_STRATEGIES.md` describes the three paths and the cross-over arithmetic.

## [0.6.1] - 2026-05-18

### Fixed

- H1 handler dispatch deferred from `on_headers_complete` to `on_message_complete` for buffered bodies — a TCP-fragmented request no longer runs the handler against a partial `$req->getBody()`. Streaming handlers (`setBodyStreamingEnabled(true)`) still dispatch at headers-complete. Test: `h1/018-tcp-fragmentation.phpt`.
- Request leak in deferred-dispatch path when parse error fires between headers-complete and message-complete (chunked body cap). `parser->owns_request` is now flipped only on actual handoff.

## [0.6.0] - 2026-05-18

### Added

- `HttpServerConfig::setBootloader(?Closure)` / `getBootloader()` — closure deep-copied into each worker, runs before task loop. Requires TrueAsync ABI v0.15+. Test: `server/core/021-bootloader.phpt`.

### Fixed

- Double-destroy in `conn_arena_free` under TLS load (re-entrant destroy via `tls_finalize_if_closing` on freed conn). Guarded by `conn->destroying` bit.

### Changed

- Asymmetric TLS BIO ring sizes: CT-in 64K→17K, PT-app back-channel 32K→17K. CT-out/PT-out unchanged. Saves ~62 KiB per TLS conn, no throughput impact.

## [0.5.3] - 2026-05-16

### Fixed

- **HTTP/2 over TLS**: `Response->setBody()` with body > 64 KiB hung
  after the initial flow-control window — buffered data_provider had
  no `write_event` subscriber, WINDOW_UPDATE never reached emit. Test:
  `h2/024-h2-tls-large-body.phpt`.

- **StaticHandler open-file cache UAF**: cache stored `content_type`
  as a borrowed pointer, but the precompressed-sidecar path passed a
  transient `zend_string` (override MIME). Next cache hit dereferenced
  freed memory → heap corruption under load (HttpArena static-h2
  lite collapsed to ~190 RPS). Cache now copies. Test:
  `static/011-static-precompressed-cache-uaf.phpt`.

- **StaticHandler sync-slurp on cache hit**: the small-file shortcut
  (≤64 KiB + cache hit) bypassed the engine and dropped
  `Content-Encoding`, `Vary`, and the override `Content-Type` for
  precompressed sidecars — browsers rendered brotli as garbage. Test:
  `static/012-static-precompressed-small-cache-headers.phpt`.

### Performance

- HttpArena `static-h2` lite: 10 RPS / death-spiral → **~163k RPS**,
  0 errored (cache UAF fix).

## [0.5.2] - 2026-05-16

### Fixed

- **Windows / MSVC build**: add `src/http_body_stream.c` to `config.w32`.
  v0.5.1 only patched the CMake unit-test build; the production NMAKE
  build still failed to link with `unresolved external symbol
  http_body_stream_{push,pop,close,dispose}` from `http_parser.obj`,
  `http2_session.obj`, and `http_request.obj`.

## [0.5.1] - 2026-05-16

### Fixed

- **Windows / MSVC build**: restore the Win32 build after the streaming
  request body merge (PR #27).
  - CMake: add `src/http_body_stream.c` and the HTTP/2 sources to the
    Win32 source list; guard TLS-only sources on `OpenSSL_FOUND`.
  - Unit tests: stop letting PHP's `win32/unistd.h` / `win32/time.h`
    shadow the CRT system headers; add the four sources that
    `http_parser.c` and `multipart_processor.c` now depend on
    (`http_body_stream.c`, `core/body_pool.c`, `http_rfc5987.c`,
    `http_param_parse.c`); add a lightweight compression-vtable stub
    for `test_compression_negotiate`; prepend `PHP_DLL_DIR` and
    `CMOCKA_DLL_DIR` to PATH for every CTest target so DLL loading
    no longer fails with 0xc0000135.

Linux / macOS behaviour is unchanged — this release is Win32-only
in terms of effect.

## [0.5.0] - 2026-05-16

### Added

- `HttpRequest::readBody(int $maxLen = 65536): ?string` — pull-based
  streaming read of the request body. Returns one parser-supplied
  chunk (H2 DATA frame, default 16 KiB; H1 llhttp on_body slice,
  bounded by the H1 read buffer = 8 KiB) per call, or `null` at EOF.
  Parks the coroutine on a per-request trigger event when the queue
  is empty. Throws `\Exception` if the stream errored (peer reset,
  size cap exceeded). `$maxLen` is reserved for a future pop-side
  coalesce — kept in the signature so the eventual wiring is binary
  compatible (issue #26).
- `HttpServerConfig::setBodyStreamingEnabled(bool): static` /
  `isBodyStreamingEnabled(): bool` — server-wide flag, default
  `false`. When enabled, H1 and H2 parsers push DATA chunks into a
  per-request FIFO instead of accumulating into `req->body`, so the
  handler can consume the body via `readBody()` without ever
  materialising the full payload in memory.

### Performance

- Streaming request body (issue #26). For handlers that opt in via
  `setBodyStreamingEnabled(true)` and consume through `readBody()`,
  the per-request RSS footprint of an in-flight upload drops from
  `~Content-Length` to roughly one parser chunk. Measured on h2load
  with 50 concurrent 20 MiB POSTs (release PHP, WSL2):
  - peak RSS: 1170 MiB → **197 MiB** (~6× reduction)
  - throughput: 36 req/s → **100 req/s** (~2.7× improvement, mostly
    because handler dispatch no longer waits for the full body)
  Buffered handlers (no opt-in) keep the previous behaviour byte for
  byte; A/B benchmarks on the H1 + H2 baseline endpoints and on the
  buffered upload path show no regression beyond WSL2 measurement
  noise. Backpressure (`llhttp_pause` + deferred
  `nghttp2_session_consume`), `readBodyChunks()` for zero-copy
  scatter reads, HTTP/3 wiring, and the mode trichotomy with
  `LogicException` are tracked as follow-ups in
  `docs/PLAN_STREAMING_INGRESS.md`.

## [0.4.0] - 2026-05-11

### Performance

- Per-thread body-buffer pool for large request bodies (≥ 1 MB). Bodies
  in this size range are allocated through zend_mm but freed back to a
  thread-local LIFO instead of being released — subsequent requests of
  the same size class reuse the slot, eliminating per-request mmap /
  munmap traffic and the kernel `mmap_lock` contention that capped
  multi-worker scaling on upload-heavy workloads. Local benchmark
  (W=8, c=128, 2 MiB POST body) goes from ~1500 RPS / 370% CPU to
  ~3300 RPS / 720% CPU (×2.2 throughput; CPU now actually scales with
  workers). Drained on `HttpServer::stop()` and on PHP RSHUTDOWN; the
  debug zend_mm leak detector sees a clean slate at module unload.
  Compression decoders (gzip / brotli / zstd request body) and the
  request destructor route releases through a single `body_release()`
  helper that recognises pool-owned slots.

### Added

- `HttpResponse::sendFile(string $path, ?SendFileOptions $options = null): void`
  — handler-driven file delivery. Records path + options on the response
  and returns immediately; the protocol's `send_static_response` op
  runs the actual transfer in the dispose phase, reusing the static
  module's open-stat-sendfile FSM (MIME detection, ETag, IMF date,
  Range, conditional GET, precompressed sidecars). Path is treated as
  trusted (handler made the access decision). Open / fstat errors
  surface as a 500 since headers aren't on the wire yet.
  After `sendFile()` the response is sealed: `setHeader` / `setStatus*` /
  `write` / `send` / `setBody` / `json` / `html` / `redirect` / `end` /
  a second `sendFile()` throw `HttpServerRuntimeException`.
  New value-object `TrueAsync\SendFileOptions` (`final readonly class`,
  named-args constructor) carries `contentType`, `disposition`
  (`SendFileDisposition::INLINE | ATTACHMENT`), `downloadName`,
  `cacheControl`, `etag`, `lastModified`, `acceptRanges`,
  `precompressed`, `conditional`, `deleteAfterSend`, `status` overrides.
  Compression middleware is bypassed for sendFile bodies (own
  delivery pipeline). HTTP/3 path is follow-up — the dispose hook
  refuses with 500 for now.

### Changed

- Static-handler PHP enum cases renamed to UPPER_CASE for project-wide
  consistency: `StaticOnMissing::{NotFound→NOT_FOUND, Next→NEXT}`,
  `StaticDotfiles::{Deny→DENY, Allow→ALLOW, Ignore→IGNORE}`,
  `StaticSymlinks::{Reject→REJECT, Follow→FOLLOW, OwnerMatch→OWNER_MATCH}`.
  Breaking for any existing user code that referenced the old casings.

### Performance

- Static file delivery — inline `open(2)` / `fstat(2)` in `send_file`
  engine (issue #13). The previous `ZEND_ASYNC_FS_OPEN` /
  `ZEND_ASYNC_IO_STAT` chain routed both syscalls through the libuv
  thread pool: one futex round-trip per request just to learn whether
  a file existed. On a warm dentry cache both syscalls are sub-µs;
  the dispatch was pure overhead. A 0-ms timer defers `engine_handle_stat`
  off the synchronous tail so on_done cannot re-enter the request
  dispatcher on its own call stack. Wins: H1 tiny 256B 19k → 35k req/s,
  H1 304 If-None-Match 24k → 123k req/s.
- Static file delivery — small-file fast path (≤ 64 KiB). libuv's
  `uv_fs_sendfile` on Linux is doubly broken for file→TCP-socket: it
  tries `copy_file_range` first (returns EINVAL on socket), then falls
  into a userspace `pread` + `write` loop inside a worker thread — no
  kernel zero-copy plus a futex round-trip per request. Files at or
  under 64 KiB are now slurped synchronously into a `zend_string` and
  emitted as one `writev(headers + body)` through libuv's per-socket
  write queue; ordering with prior writes is then libuv's problem.
  Files above 64 KiB stay on the (broken-but-functional) sendfile
  path. Wins on top of the inline-syscall change: H1 tiny → 103k req/s
  (×2.9), H1 small 16K → 73k (×1.9), H2 tiny → 154k (×4.4), H2 small
  → 73k (×2.7).

### Fixed

- HTTP/2 single byte-range delivery served bytes from offset 0 of the
  file regardless of the requested range start. The H2 body FSM stored
  `body_offset` in its state but never applied it to the file: the
  buffered read path (`ZEND_ASYNC_IO_READ`) uses the fd's tracked
  position, which is 0 right after open. Seek the io once before the
  first read when `body_offset > 0`. H1 was unaffected (sendfile path
  passes the offset explicitly to the syscall).
  Closes pre-existing failure of `tests/phpt/server/static/012-static-h2`.
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

[Unreleased]: https://github.com/true-async/server/compare/v0.9.3...HEAD
[0.9.3]: https://github.com/true-async/server/compare/v0.9.2...v0.9.3
[0.9.2]: https://github.com/true-async/server/compare/v0.9.1...v0.9.2
[0.9.1]: https://github.com/true-async/server/compare/v0.9.0...v0.9.1
[0.9.0]: https://github.com/true-async/server/compare/v0.8.1...v0.9.0
[0.8.1]: https://github.com/true-async/server/compare/v0.8.0...v0.8.1
[0.8.0]: https://github.com/true-async/server/compare/v0.7.3...v0.8.0
[0.7.2]: https://github.com/true-async/server/compare/v0.7.1...v0.7.2
[0.7.1]: https://github.com/true-async/server/compare/v0.7.0...v0.7.1
[0.7.0]: https://github.com/true-async/server/compare/v0.6.7...v0.7.0
[0.6.7]: https://github.com/true-async/server/compare/v0.6.6...v0.6.7
[0.6.6]: https://github.com/true-async/server/compare/v0.6.5...v0.6.6
[0.6.5]: https://github.com/true-async/server/compare/v0.6.4...v0.6.5
[0.6.4]: https://github.com/true-async/server/compare/v0.6.3...v0.6.4
[0.6.3]: https://github.com/true-async/server/compare/v0.6.2...v0.6.3
[0.6.2]: https://github.com/true-async/server/compare/v0.6.1...v0.6.2
[0.6.1]: https://github.com/true-async/server/compare/v0.6.0...v0.6.1
[0.6.0]: https://github.com/true-async/server/compare/v0.5.3...v0.6.0
[0.5.3]: https://github.com/true-async/server/compare/v0.5.2...v0.5.3
[0.5.2]: https://github.com/true-async/server/compare/v0.5.1...v0.5.2
[0.5.1]: https://github.com/true-async/server/compare/v0.5.0...v0.5.1
[0.5.0]: https://github.com/true-async/server/compare/v0.4.2...v0.5.0
[0.4.0]: https://github.com/true-async/server/compare/v0.3.2...v0.4.0
[0.3.2]: https://github.com/true-async/server/compare/v0.3.1...v0.3.2
[0.3.1]: https://github.com/true-async/server/compare/v0.3.0...v0.3.1
[0.3.0]: https://github.com/true-async/server/compare/v0.2.0...v0.3.0
[0.2.0]: https://github.com/true-async/server/compare/v0.1.5...v0.2.0
[0.1.0]: https://github.com/true-async/server/releases/tag/v0.1.0
