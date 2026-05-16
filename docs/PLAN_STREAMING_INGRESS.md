# Streaming request body — design + implementation plan

Replaces the current "buffer-the-whole-body-then-dispatch-handler" path
with an optional pull-based streaming API. Unblocks HttpArena `upload`
profile (currently 20 MiB body materialized per request → ~20 GB peak
at 1000 concurrent uploads) and enables proxy / direct-to-storage /
incremental-hash use cases that buffer-everything makes impossible.

This document is the load-bearing context for resuming the work after a
clean session. Read it end-to-end before touching code.

---

## 1. Surface (PHP API)

Two new methods on `TrueAsync\HttpRequest`. One new config setter on
`TrueAsync\HttpServerConfig`. Existing API (`getBody()`,
`awaitBody()`, `getFiles()`, `getPost()`) — **no signature changes**,
only lazy-mode lock semantics added.

### `HttpRequest`

```php
namespace TrueAsync;

class HttpRequest {
    // ─── existing buffered API (unchanged signature) ───
    public function getBody(): string {}
    public function awaitBody(): static {}

    // ─── existing multipart-parsed API (unchanged signature) ───
    public function getFiles(): array {}
    public function getPost(): array {}

    // ─── NEW: streaming primitives ───

    /**
     * Read up to $maxLen bytes from the request body.
     *
     *  - string (1..$maxLen bytes) — data available
     *  - null                      — EOF (idempotent: subsequent calls also null)
     *
     * Parks the coroutine until at least one byte is available or EOF.
     * Each returned chunk is released immediately; server backpressure
     * (H1 pause, H2/H3 flow-control) applies automatically until the
     * next call drains the queue.
     *
     * $maxLen <= 0 clamps to 65536.
     *
     * @throws \LogicException  if buffered/multipart mode already engaged
     * @throws \RuntimeException connection dropped / max_body_size exceeded
     */
    public function readBody(int $maxLen = 65536): ?string {}

    /**
     * Low-level zero-copy variant: returns an array of zend_string
     * chunks in arrival order, total size ≤ $maxLen. Same semantics as
     * readBody() otherwise. Useful for proxy / hash-while-receive
     * patterns where the coalescing memcpy of readBody() is the cost
     * being avoided.
     *
     * Empty array is never returned (parks like readBody).
     */
    public function readBodyChunks(int $maxLen = 65536): ?array {}
}
```

### `HttpServerConfig`

```php
/**
 * Per-stream backpressure watermark in bytes for streaming bodies.
 * When undrained chunks in the queue exceed this size, the parser is
 * paused (H1) / WINDOW_UPDATE is withheld (H2) / stream offset is
 * withheld (H3) until readBody() drains. Applies only in streaming
 * mode; buffered mode (getBody()) still uses setMaxBodySize() as the
 * single cap.
 *
 * Default: 262144 (256 KiB). Valid: 16384 .. setMaxBodySize().
 *
 * @return static
 */
public function setMaxBodyQueueBytes(int $bytes): static {}
public function getMaxBodyQueueBytes(): int {}
```

### Mode trichotomy — first call wins

| first call            | mode               | subsequently allowed                    |
|-----------------------|--------------------|-----------------------------------------|
| `getBody/awaitBody`   | **buffered**       | `getBody()` only                        |
| `readBody*`           | **streaming**      | `readBody()`, `readBodyChunks()` (interchangeable) |
| `getFiles/getPost`    | **multipart-parsed** | `getFiles()`, `getPost()`             |

Mismatched calls after lock → `LogicException`. Why: the bytes are
consumed exactly once; pretending otherwise hides bugs.

Why lazy-switch (no explicit `enableBodyStreaming()`): order-of-
operations footgun ("forgot to call enable → silently buffered → OOM
on 20 MiB upload"). The mode is implied by what you ask for.

---

## 2. Internal queue — `zend_async_channel_t`

Use the public `ZEND_ASYNC_NEW_CHANNEL(buffer_size=16, resizable=false,
thread_safe=false)` from `Zend/zend_async_API.h:2544`. Backed internally
by `circular_buffer` (power-of-2 cap, `& (cap-1)` masking, inline
`push_ptr`/`pop_ptr` zero-copy specialization).

### Why channel and not roll-your-own ring

- `zend_async_channel_t` inherits `zend_async_event_t` — `receive()`
  parks the coroutine natively; no separate `body_chunk_event` /
  trigger / wakeup wiring.
- Cancellation semantics already correct: coroutine cancelled while
  parked in `receive()` → channel cleans up its waiter list.
- `close()` + EOF semantics: `receive()` on closed channel drains
  remaining items first, then returns false. Confirms with
  `on_message_complete` calling `channel->close()`.
- One codepath shared with ext-async pool/microtasks/scheduler. Same
  primitive, less surface for bugs.

### Sizing

| param | value | rationale |
|---|---|---|
| `buffer_size` (slots) | **16** | H2 default frame = 16 KiB; 16 slots × 16 KiB = 256 KiB = byte-watermark default → both limits fire together; 8 slots would slot-limit at 128 KiB on H2, leaving byte-watermark dead |
| `max_body_queue_bytes` (default) | **262144** (256 KiB) | balances backpressure cadence vs throughput; less = chatter; more = wasted memory per stream |
| zval overhead per chunk | 16 B | 16 × 16 B = 256 B per request — negligible |

### Per-stream double limit

1. Slot count (16) — secondary guard against pathological frame sizes
2. `bytes_queued` counter (new field on `http_request_t`) — primary
   limiter, checked before push: `bytes_queued + new_chunk > watermark`
   → defer push, apply backpressure
3. `max_body_size` (existing) — cumulative absolute cap;
   `bytes_consumed + bytes_queued + new_chunk > max_body_size` → 413 +
   close stream

Slot count fires only on a misbehaving peer that ignored our advertised
`SETTINGS_MAX_FRAME_SIZE` (see §4).

---

## 3. Request structure changes

In `include/http_request.h`:

```c
typedef enum {
    HTTP_REQ_BODY_MODE_UNSELECTED = 0,  /* nothing called yet */
    HTTP_REQ_BODY_MODE_BUFFERED,        /* getBody/awaitBody */
    HTTP_REQ_BODY_MODE_STREAMING,       /* readBody/readBodyChunks */
    HTTP_REQ_BODY_MODE_MULTIPART,       /* getFiles/getPost */
} http_request_body_mode_t;

typedef struct {
    /* ─── existing fields ─── */

    /* ─── new ─── */
    http_request_body_mode_t body_mode;

    /* Streaming queue. Allocated lazily on first readBody().
     * Lifetime tied to req; closed on EOF / error / req dispose. */
    zend_async_channel_t    *body_channel;
    size_t                   body_bytes_queued;
    size_t                   body_bytes_consumed;
    bool                     body_eof;       /* parser saw message-complete */
    bool                     body_paused;    /* backpressure currently engaged */
} http_request_t;
```

`use_multipart` / `multipart_proc` stay where they are, but their
**activation moves from `on_headers_complete` to first call of
`getFiles`/`getPost`** (lazy as per mode trichotomy).

---

## 4. Per-transport integration

### HTTP/1.1 (`src/http1/http_parser.c`)

`on_body`:
```
if (req->body_mode == HTTP_REQ_BODY_MODE_STREAMING) {
    if (req->body_bytes_queued + length > watermark) {
        llhttp_pause(parser);            /* backpressure */
        req->body_paused = true;
        // re-feed at resume time; llhttp will replay paused buffer
    }
    zend_string *chunk = zend_string_init(at, length, 0);
    zval z; ZVAL_STR(&z, chunk);
    channel->send(channel, &z);
    req->body_bytes_queued += length;
    return 0;
}
/* MULTIPART / BUFFERED / UNSELECTED — current accumulator path */
```

`on_message_complete`:
```
if (req->body_channel) {
    req->body_eof = true;
    req->body_channel->close(req->body_channel);
}
```

In `readBody()` C-impl, after a successful `receive()`:
```
req->body_bytes_consumed += chunk_len;
req->body_bytes_queued   -= chunk_len;
if (req->body_paused && req->body_bytes_queued < (watermark / 2)) {
    llhttp_resume(parser);
    req->body_paused = false;
}
```

Resume uses low-water-at-50% to avoid pause/resume thrashing.

### HTTP/2 (`src/http2/http2_session.c`)

`on_data_chunk_recv_callback`:
```
if (stream->req->body_mode == HTTP_REQ_BODY_MODE_STREAMING) {
    push to channel as above
    /* do NOT call nghttp2_session_consume — withholds WINDOW_UPDATE */
    return 0;
}
/* legacy buffered path */
```

In `readBody()` drain:
```
req->body_bytes_consumed += len;
nghttp2_session_consume(session, stream_id, len);  /* releases flow ctl */
```

**SETTINGS adjustment** — in `src/http2/http2_strategy.c` connection
init: advertise `SETTINGS_MAX_FRAME_SIZE = max_body_queue_bytes`.
Default 256 KiB, well within the spec-allowed range (16 KiB .. 16 MiB).
This guarantees no single frame can blow the watermark in one shot.
Compliant clients respect; non-compliant → `FRAME_SIZE_ERROR` per RFC
7540 §6.5.2.

### HTTP/3 (`src/http3/http3_stream.c`)

`on_recv_data`:
```
if (req->body_mode == HTTP_REQ_BODY_MODE_STREAMING) {
    push to channel
    /* defer ngtcp2_conn_extend_max_stream_offset */
    return 0;
}
```

In drain, after `receive()`:
```
ngtcp2_conn_extend_max_stream_offset(conn, stream_id, len);
ngtcp2_conn_extend_max_offset(conn, len);
```

H3 has no equivalent of `SETTINGS_MAX_FRAME_SIZE`; rely on initial
max_stream_data sized to watermark.

---

## 5. Edge cases — explicit semantics

| case | behavior |
|---|---|
| handler returns without consuming | server drains channel into /dev/null (H1) or RST_STREAM(NO_ERROR) (H2/H3); request dispose closes channel |
| `readBody()` after `getBody()` (or vice-versa) | `LogicException` with mode name in message |
| `readBody()` after `getFiles()` (or vice-versa) | `LogicException` |
| connection reset mid-body | parser sees EOF prematurely → `body_eof=true` + close channel + set error flag; next `readBody()` throws `RuntimeException` |
| `max_body_size` exceeded | 413 sent (H1) / RST_STREAM (H2/H3); channel closed with error flag; next `readBody()` throws `RuntimeException` |
| `readBody()` after EOF | returns `null` idempotently (channel.receive() returns false on closed-and-empty) |
| coroutine cancelled while parked | channel removes waiter, releases reference; on resume CancellationException propagates |
| `hasBody()` is false | `readBody()` returns `null` immediately (no channel allocated) |
| `$maxLen > queued bytes` | returns up to all queued bytes (short read is normal) |
| early arrival before first userland call | chunks accumulate in channel (mode UNSELECTED); first call decides — STREAMING reads them; BUFFERED drains them into smart_str then proceeds in legacy path |
| multipart body opted into streaming | `use_multipart` not flipped on `on_headers_complete`; user gets raw multipart bytes; user can run own parser; `getFiles()`/`getPost()` then throws `LogicException` |
| pause/resume on chunked H1 | llhttp_resume replays its internal buffer; no data loss |
| short channel buffer overflow | slot-limit (16) trips only if peer ignored SETTINGS_MAX_FRAME_SIZE; treat as protocol violation: GOAWAY (H2) / CONNECTION_CLOSE (H3) / 400+close (H1) |

---

## 6. Performance notes

### What's saved

Per-request peak memory for a streaming upload:
- before: ~`Content-Length` (single zend_string accumulator)
- after: ~`max_body_queue_bytes` + 64 KiB return-string buffer (~320 KiB total)

20 MiB upload, 1000 concurrent streams:
- before: ~20 GiB peak
- after: ~320 MiB peak — **60× reduction**

### What's NOT saved

For handlers that still call `getBody()` — no change. They opted into
the buffered path; we honor it. The streaming infrastructure adds zero
overhead to that code path (mode check is one branch on first body
access).

### Hot-path concerns

- Channel push/pop: inline `circular_buffer_push_ptr` /
  `_pop_ptr`, ~10 ns each on modern CPU. Negligible vs syscall +
  parser cost.
- zval boxing per chunk: 16 B memcpy + ZVAL_STR; ~5 ns.
- Coalescing memcpy in `readBody()`: bounded by watermark (256 KiB),
  ~5 µs on DDR4; orders of magnitude under syscall and network costs.
- `readBodyChunks()` skips the coalescing memcpy entirely for the
  zero-copy case.

### Backpressure correctness

The low-water-at-50% resume threshold is borrowed from TCP RWND
auto-tuning idiom. Single-watermark resume (resume the moment
`queue_bytes < watermark`) causes pause/resume chatter under steady
traffic when one chunk pop barely dips below the limit and the next
push trips it again. 50% gap is the standard cure.

### Concurrency

- Per-request channel; no cross-stream sharing → `thread_safe=false`
  is safe.
- Same-thread producer (parser callback on loop thread) + consumer
  (handler coroutine on same loop thread). Channel internals stay
  lockless on this configuration.
- No global state added.

---

## 7. Coding-standard checks

- C: see `docs/CODING_STANDARDS.md`. `const` on pointers and arguments
  by default. `UNEXPECTED`/`EXPECTED` on the mode-branch in `on_body`.
  `static` on every file-local function.
- No PHP `call_user_func` in body path. Handler dispatch unchanged.
- `emalloc` for per-request streaming state; `pemalloc` only for
  config (watermark setting on the server-shared config).
- Bailout firewall not needed — streaming path is pure C; PHP-VM
  re-enters only inside the handler, which the existing dispatch
  firewall already guards.

---

## 8. PR breakdown

| PR | Scope | Blocks |
|---|---|---|
| **#1** | `setMaxBodyQueueBytes` / getter on HttpServerConfig + arginfo. Plumbing to `http_server_config_t`. PHPT: validation, locked-after-start. | — |
| **#2** | `http_request_t` body-mode trichotomy + channel field. Lazy `multipart_proc` activation moved from `on_headers_complete` to first `getFiles/getPost`. `getBody/awaitBody` mode-lock + `LogicException`. PHPT: mode mutex, multipart-then-getBody throws, etc. | #1 |
| **#3** | `readBody()` C-impl + stub + arginfo. H1 integration: `on_body` mode-branch, llhttp pause/resume with 50% low-water. PHPT: basic stream, EOF returns null, RuntimeException on connection drop, max_body_size cap. | #2 |
| **#4** | H2 integration: `on_data_chunk_recv_callback` branch, deferred `nghttp2_session_consume`. `SETTINGS_MAX_FRAME_SIZE` advertisement set to watermark. PHPT: H2 stream + WINDOW_UPDATE behavior, oversized-frame from peer rejected. | #3 |
| **#5** | H3 integration: `on_recv_data` branch, deferred `extend_max_stream_offset`. `initial_max_stream_data` sized to watermark. PHPT: same matrix on H3. | #3 |
| **#6** | `readBodyChunks()` C-impl + stub + arginfo. Mostly free given #3 (skip coalesce). PHPT: array return, mixed alternation with `readBody()` on same request OK. | #3 |
| **#7** | HttpArena `entry.php` rewrite: `/upload` switched to `while (($c = $req->readBody()) !== null) $bytes += strlen($c);`. Measure RSS at 16-conn × 30s upload-mixed load. Target: peak < 500 MiB (vs current ~900 MiB on round 3). | #3–#6 |

PRs #4 and #5 are independent of each other and can land in either order.

---

## 9. Out of scope (for this work)

- **Streaming response body** — already done, see `project_streaming_completion_loop.md`. Not touched here.
- **Backpressure feedback to handler** (a `sendable()`-like predicate
  on the request side) — userland can check `getContentLength()`
  against `body_bytes_consumed` if needed; not adding new API.
- **Iterator wrapper** (`getBodyStream(): \Generator`) — userland
  one-liner over `readBody()`; not blessed into the core API.
- **PSR-7 `StreamInterface` wrapper** — userland concern.
- **Reading body from C without going through PHP** (e.g. for a future
  C-level proxy module) — possible by directly draining the channel
  from C; revisit when there's a concrete consumer.
- **Per-connection (not per-stream) global watermark** — H2 connection
  flow-control window is separate from per-stream; current default
  (64 KiB initial, expanded by `WINDOW_UPDATE`) is fine. If aggregate
  pressure matters, separate PR.

---

## 10. Status — что сделано, что осталось

(empty — work not started)
