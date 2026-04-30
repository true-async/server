# WebSocket support — implementation plan

RFC 6455 (WebSocket Protocol) and RFC 8441 (WebSocket over HTTP/2) on the
TrueAsync Server core. Tracks issue #2.

This document fixes the design decisions made before implementation and
explains the *why* behind each one. Code-level conventions live in
[`CODING_STANDARDS.md`](CODING_STANDARDS.md).

---

## 1. Dependency: bundled wslay

WebSocket frame parsing is delegated to [wslay](https://github.com/tatsuhiro-t/wslay)
(MIT, vendored at `deps/wslay/`, see `deps/wslay/UPSTREAM.md`).

**Why not hand-roll.** The frame format is compact, but the security-relevant
parts — masking, fragmentation reassembly, UTF-8 validation across fragment
boundaries, control-frame invariants per RFC 6455 §5 — are easy to get
subtly wrong, and the resulting bugs are exploitable. wslay is by the same
author as nghttp2/ngtcp2/nghttp3 (already linked here), uses the same
callback idiom we already integrate with, and is "complete" — its release
cadence is slow because RFC 6455 doesn't change, not because it is
unmaintained.

**Why not uWebSockets.** C++, header-only, tightly coupled to its own event
loop. Would force C++ into an otherwise pure-C extension for no clean win.

**What wslay does NOT do (and we accept):**
- HTTP-level handshake (we do this on top of llhttp / nghttp2).
- Network I/O (wslay calls `send_callback` / `recv_callback` — we plug our
  TLS / socket pipeline in).
- permessage-deflate (RFC 7692). Out of MVP scope; separate later PR.

Build flag: `--enable-websocket` (default `yes`). Defines `HAVE_WSLAY` and
`HAVE_HTTP_SERVER_WEBSOCKET`. `--disable-websocket` drops every wslay
source from the build cleanly (verified: 0 wslay symbols in the resulting
`.so`).

---

## 2. Architectural fit

### 2.1 Strategy pattern reuse

A new `http_protocol_strategy_websocket_create()` lives next to the H1 and
H2 strategies. Its `feed()` parses incoming frames; its `send_response`
slot is unused (there is no HTTP response after the upgrade); its
`cleanup()` sends a CLOSE.

Connection-level switching at runtime: after a successful upgrade, the
connection layer destroys the old strategy and installs the WS one
(`conn->strategy = ws; conn->protocol_type = HTTP_PROTOCOL_WEBSOCKET`).
The read loop in `http_connection.c` is already protocol-agnostic
(`strategy->feed` is the only entry point); the TLS pipeline is also
agnostic (`tls_layer.c` delivers plaintext regardless of who consumes it).

### 2.2 Streaming ops reuse

A third `http_response_stream_ops_t` implementation — `ws_stream_ops` —
sits next to `h1_stream_ops` and the H2 stream ops. It frames application
payloads into WS frames and applies backpressure through the same
`stream_write_buffer_bytes` knob used by `HttpResponse::send()`.

### 2.3 Handler coroutine lifecycle

The handler coroutine spawned by `http_connection_dispatch_request` simply
keeps running after the 101 response — there is no second coroutine. It
holds the connection alive via `handler_refcount` (the same mechanism
already battle-tested for HTTP/2 multiplex). When the handler returns,
`refcount` drops to zero and the standard dispose path closes the WS
gracefully (1000 Normal — see §6.9).

### 2.4 Send pipeline: flusher-role pattern

WS-send mirrors the discipline already used in `http_connection_tls.c` for
TLS plaintext flushing:

- `$ws->send()` from any coroutine serializes the frame and atomically
  enqueues it. **It does no I/O.**
- A per-connection bit `ws_flushing : 1` indicates whether some producer
  is currently flushing. The first producer that finds it clear *becomes*
  the flusher: it dequeues frames and writes them through the suspending
  `send_raw` path, one whole frame at a time. Other producers find the
  bit set and just enqueue + return.
- Suspension can happen *between* frames (when the kernel send buffer
  fills), never *inside* one — there is no other writer to interleave
  with by construction.

This gives us multi-producer-safe `send()` with zero new locking
primitives, on the same single-threaded coroutine model used everywhere
else in the codebase.

---

## 3. Handshake

### 3.1 HTTP/1.1 Upgrade (PR-1 scope)

llhttp already pauses on `Upgrade: websocket` with `HPE_PAUSED_UPGRADE`.
The dispatch path validates `Sec-WebSocket-Version: 13`, `Sec-WebSocket-Key`
presence, optional subprotocol/extension offers, gives the handler a
`WebSocketUpgrade` handle, and on accept emits a `101 Switching Protocols`
with the computed `Sec-WebSocket-Accept`.

The bytes already buffered after the headers (because the client may start
sending frames immediately) are passed to the WS strategy's first
`feed()`. `consumed_out` from `http_parser_execute` already gives us this
boundary.

Rejection paths:
- bad / missing version → `426 Upgrade Required` with `Sec-WebSocket-Version: 13`
- missing Key/Connection → `400`
- handler-driven reject → status from `WebSocketUpgrade::reject()`

### 3.2 HTTP/2 Extended CONNECT (PR-2 scope)

RFC 8441: `:protocol = websocket` inside one H2 stream, gated by
`SETTINGS_ENABLE_CONNECT_PROTOCOL = 1` advertised by the server only when
a WS handler is registered. WS-frames live inside DATA frames; the same
`ws_stream_ops` and parser are used. Masking still applies.

Deferred to a separate PR for risk isolation. Public PHP API is designed
to stay identical so the H2 path lands as a purely internal addition.

---

## 4. Public PHP API

```php
final class WebSocket {
    // === recv (single-reader) ===
    public function recv(): ?WebSocketMessage;          // suspend; null on graceful close
    // (IteratorAggregate — foreach $ws as $msg also works)

    // === send (multi-producer-safe) ===
    public function send(string $text): void;           // text frame (UTF-8)
    public function sendBinary(string $data): void;     // binary frame

    // === control ===
    public function ping(string $payload = ''): void;
    public function close(WebSocketCloseCode|int $code = WebSocketCloseCode::Normal,
                          string $reason = ''): void;

    // === state ===
    public function isClosed(): bool;
    public function getSubprotocol(): ?string;
    public function getRemoteAddress(): string;
}

final class WebSocketMessage {
    public readonly string $data;
    public readonly bool   $binary;       // false = text, UTF-8-validated
}

final class WebSocketUpgrade {
    public function reject(int $status, string $reason = ''): void;
    public function setSubprotocol(string $name): void;
    public function getOfferedSubprotocols(): array;
    public function getOfferedExtensions(): array;
}

enum WebSocketCloseCode: int {
    case Normal              = 1000;
    case GoingAway           = 1001;
    case ProtocolError       = 1002;
    case UnsupportedData     = 1003;
    case NoStatus            = 1005;
    case AbnormalClosure     = 1006;
    case InvalidFramePayload = 1007;
    case PolicyViolation     = 1008;
    case MessageTooBig       = 1009;
    case MandatoryExtension  = 1010;
    case InternalServerError = 1011;
    case TlsHandshake        = 1015;
}

final class WebSocketClosedException extends \RuntimeException {
    public readonly int    $code;
    public readonly string $reason;
}

final class WebSocketBackpressureException extends \RuntimeException {}
final class WebSocketConcurrentReadException extends \LogicException {}
```

Handler registration arity-driven:

```php
$server->addWebSocketHandler(function (WebSocket $ws, HttpRequest $req): void {
    while (($msg = $ws->recv()) !== null) {
        $ws->send("echo: {$msg->data}");
    }
});

$server->addWebSocketHandler(function (WebSocket $ws, HttpRequest $req,
                                       WebSocketUpgrade $u): void {
    if (!$req->hasHeader('Authorization')) {
        $u->reject(401);
        return;
    }
    $u->setSubprotocol('chat.v2');
    // ...
});
```

The 3-arg form is detected via `ReflectionFunction` at registration time
and the upgrade object is built only when needed.

---

## 5. Configuration

New fields on `HttpServerConfig`, all `_ms` suffix to match the existing
convention:

| Field | Default | Meaning |
|---|---|---|
| `ws_ping_interval_ms`   | 30000 | server-initiated ping cadence; 0 = disabled |
| `ws_pong_timeout_ms`    | 60000 | pong deadline; 0 = disabled (with ping); >0 → close 1001 |
| `ws_max_message_size`   | 1 MiB | reassembled-message cap; oversize → close 1009 |
| `ws_max_frame_size`     | 1 MiB | per-frame cap (defence against fragment-flood) |

Setters on `HttpServerConfig` mirror existing `setReadTimeout` / etc.
After `lock()` the values are mirrored into `http_server_shared_config_t`
just like the other knobs. Read by the WS strategy at attach time.

`stream_write_buffer_bytes` (existing) is reused as the WS send-queue
high-watermark.

`read_timeout_ms` is **disabled per-connection after upgrade** — keepalive
ping/pong is the correct idle-detection mechanism for WS, not the
HTTP-request timeout. Otherwise every long-lived WS would die after 30s.

---

## 6. Design decisions (with rationale)

### 6.1 Single class, no `WebSocketH1` / `WebSocketH2` split
Differences are transport-level only; from the application's perspective
both expose the same full-duplex message stream.

### 6.2 Pull API, not push (`onMessage` callbacks)
TrueAsync gives PHP true coroutines; pull is the idiomatic shape under
that model (Python `asyncio.websockets`, Go gorilla, amphp/websocket,
tokio-tungstenite all converge here). Push is what languages without
coroutines fall back to.

### 6.3 `recv()` returns `?WebSocketMessage`, not throws on graceful close
Graceful close is normal loop termination. Throw is for protocol errors
and abnormal close only — distinguishes "expected end" from "bug".

### 6.4 Separate `sendBinary()`, not `send($data, bool $binary)`
Binary-ness is semantic, not a flag. Reduces accidental UTF-8-validation
failures on the receiving side.

### 6.5 Auto-fragmentation, no manual fragment API
wslay reassembles fragments; PHP only sees whole `WebSocketMessage`s.
Outbound: payloads larger than `ws_max_frame_size` are auto-split by the
strategy. If a stream-large-payload use case ever appears, add
`recvStream()` then; YAGNI now.

### 6.6 Auto ping/pong (control-frames invisible to PHP)
Inbound PING → server sends PONG without surfacing. Inbound PONG resets
the keepalive deadline. The handler sees only data frames. Explicit
`$ws->ping()` exists for the rare "I want to verify liveness right now"
case but is not required for ordinary keepalive.

### 6.7 Combined upgrade + connection handler
Single registration point, optional 3rd `WebSocketUpgrade` parameter for
reject / subprotocol negotiation. amphp's hard-won pattern; cleaner than
splitting `onUpgrade` and `onConnection`.

### 6.8 Backpressure: enqueue → suspend → throw
Fast path: synchronous enqueue (zero cost on 99% of calls).
Cold path A (queue above high-watermark): suspend the producer until
drain.
Cold path B (drain still hasn't happened by `write_timeout_ms`):
`WebSocketBackpressureException` so the handler can decide drop / close /
retry. Prevents slow-loris-on-write from holding a coroutine forever
(same threat that `write_timer` handles for ordinary HTTP responses).

### 6.9 `send()` multi-producer-safe, `recv()` single-reader-enforced
Multi-producer is a real use case (broadcast / pub-sub fanout). See §2.4
for the pattern.

Multi-reader has no clean semantics — round-robin loses messages, fan-out
needs per-reader buffers — and no real use case. Second concurrent
`recv()` throws `WebSocketConcurrentReadException`. Pit-of-success: code
that shouldn't work doesn't, loudly and explicitly.

### 6.10 Handler return = auto-close 1000
Lifecycle bound to the handler coroutine. Aligns with amphp / Python
websockets / Ratchet. Broadcast is unaffected: subscribers hold PHP refs
to `$ws`, and `send()` enqueues without holding the handler open.

### 6.11 `WebSocketCloseCode` enum, `int` accepted for 4000-4999
Standard codes get type safety and IDE discoverability. Application-
specific codes (RFC 6455 §7.4.2) stay open via the union type.

---

## 7. Testing

- `tests/phpt/websocket/` — handshake (valid, bad version, bad key),
  echo, fragmentation, ping/pong, close-handshake, subprotocol,
  reject paths, backpressure, concurrent send, concurrent recv (must
  throw).
- **Autobahn TestSuite** — `e2e/autobahn/` runner that boots the
  PHP server and runs the upstream container. Required cases (1–7) gate
  CI; full run is nightly.
- **Fuzzer** — `fuzz/fuzz_ws_frame.c` over wslay's `wslay_event_recv`
  with random byte streams. Even though wslay is itself fuzz-tested
  upstream, our integration (read-buffer interaction, callback-driven
  state) needs its own coverage.
- ASan / MSan inherited from existing build matrix.

---

## 8. Rollout

1. **PR-1 (this branch).** Vendoring + strategy + frame I/O + H1
   handshake + PHP API + phpt tests + Autobahn smoke. Ship with WS
   advertised "experimental" in README until PR-3 lands.
2. **PR-2.** HTTP/2 Extended CONNECT (RFC 8441). Pure internal addition.
3. **PR-3.** permessage-deflate (RFC 7692). Behind a separate config
   flag, default off. Bomb-of-decompression mitigation: enforce
   `ws_max_message_size` *before* and *after* inflate independently.
4. **PR-4.** Telemetry counters (active WS, frames in/out by opcode,
   control-frame ratio) + drain integration (CLOSE 1001 on
   `max_connection_age_ms`).
