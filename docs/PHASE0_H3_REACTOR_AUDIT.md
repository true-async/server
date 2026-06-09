# Phase 0 — H3 reactor sync-burst audit (#80)

Companion to `docs/PLAN_REACTOR_POOL.md`, Phase 0. Catalogues every place the
HTTP/3 transport reactor does synchronous, potentially-unbounded CPU or a
blocking call *without a yield point*, so we know what the reactor-pool split
(Phases 1–3) actually has to move off the transport thread.

All line numbers verified against the tree at the time of writing
(branch `81-…queue…`). Re-check before quoting — H3 files move around.

## The model today: one thread is both reactor and worker

There is no transport/worker split yet. A single worker OS thread runs the
libuv reactor *and* the PHP handler coroutines. The scheduler is cooperative,
so on that thread:

- **"reactor-blocking"** = a synchronous span with **no yield**: it runs to
  completion before control returns to libuv, so ACK/PTO for *every* live QUIC
  connection is delayed by its full duration.
- **"coroutine-safe"** = runs inside a handler coroutine that **can** `await`;
  at each await the reactor regains control and can flush ACKs. A coroutine
  that never awaits (pure CPU) is *not* safe — it monopolises the thread just
  like reactor code, it merely *could* have yielded.

This is exactly why #80 is QUIC-specific: on TCP the kernel ACKs regardless of
what userspace is doing; on QUIC the ACK clock is our reactor.

## Reactor iteration boundaries (where the watchdog measures)

| Boundary | File | Role |
|---|---|---|
| `http3_listener_poll_cb` | `src/http3/http3_listener.c:443` | readable wakeup: `recvmmsg` batches → per-datagram `http3_connection_dispatch` → `flush_dirty`. The unit the tick-latency histogram times. |
| `timer_fire_cb` | `src/http3/http3_io.c:54` | retransmission/ACK-delay/PTO/idle timer → `handle_expiry` + `drain_out`. The per-conn timer-late signal measures how late this fired vs its armed deadline. |
| `h3_dispose_tail` drain | `src/http3/http3_dispatch.c:441` | after a handler coroutine completes, drains its response on the same tick. |

## Reactor-thread synchronous sites

Ordered by how much they worry me for ACK timing.

### 1. Buffered response compression — UNBOUNDED, no yield  ⚠️ primary offload candidate

- `http_compression_apply_buffered(resp_obj)` — `src/http3/http3_callbacks.c:535`,
  inside `http3_stream_submit_response`.
- Reached from `h3_handler_coroutine_dispose` (buffered path,
  `http3_dispatch.c:574`) — i.e. **after** the handler coroutine returns, in
  scheduler/dispose context on the reactor thread. There is no await between
  "handler returned a 2 MB body" and "gzip/brotli/zstd it on the reactor".
- gzip/brotli deflate is O(body size) and opaque; a multi-MB body is tens of
  ms of pure CPU with the reactor heads-down. **This is the cleanest thing to
  move off-thread in Phase 1.**
- The streaming path (`submit_response(streaming=true)`) does *not* hit this —
  compression there is the per-chunk stream wrapper, which rides the
  flow-control backpressure suspend (see #8). Only the **buffered** (setBody/
  json/end) path is exposed.

### 2. Inbound body decompression — synchronous loop, runs on reactor (in coroutine)

- `http_compression_decode_request_body(...)` — `src/http3/http3_dispatch.c:324`,
  in `h3_handler_coroutine_entry`.
- Technically inside the handler coroutine, but the inflate loop itself has no
  yield, so while it runs it burns reactor CPU. Bounded by the anti-bomb
  post-decompress cap (`src/compression/http_compression_request.c`), so it
  cannot run truly unbounded, but a near-cap decompress is still a multi-ms
  span. Secondary offload candidate.

### 3. `drain_out` — ngtcp2/nghttp3 writev loop — bounded by iter cap

- `http3_connection_drain_out` — `src/http3/http3_io.c:194`.
- The send loop (nghttp3 framing → ngtcp2 encrypt/pack → GSO sendmsg) is
  capped at `H3_DRAIN_ITER_CAP = 4096` packets/call (`http3_io.c:246`,
  counter `quic_drain_iter_cap_hit`). 4096 × MTU ≈ 5 MiB is far above a
  legitimate single-tick burst, but it is still a synchronous span and the
  per-packet TLS encrypt is real CPU. Bounded, on-reactor, no intra-loop yield.

### 4. Response header QPACK encode — bounded by header count

- `nghttp3_conn_submit_response` — `src/http3/http3_callbacks.c:621`, plus the
  single-pass header flatten + `h3_nv_push` overflow promotion
  (`callbacks.c:558+`). QPACK-encodes response headers synchronously. Bounded
  by the header count (scratch covers ≤32; overflow promotes to heap with a
  hard cap). Low risk in practice; listed for completeness.

### 5. Request header decode / assembly — bounded, per-field allocation

- `h3_recv_header_cb` — `src/http3/http3_callbacks.c:230`; stores each field
  via `h3_store_header_value` → `zend_string_init` (`callbacks.c:125-126`,
  `:271`, `:279`). Runs as an nghttp3 read callback inside
  `ngtcp2_conn_read_pkt` on the reactor. One alloc per header; bounded by
  nghttp3's own header-list size limit plus our caps. O(headers), on-reactor.

### 6. Request body assembly — BOUNDED (not unbounded)

- `h3_recv_data_cb` — `src/http3/http3_callbacks.c:335-368`.
  `smart_str_appendl` per DATA frame, **pre-sized** from Content-Length
  (`:364`) and **hard-capped** at `HTTP3_MAX_BODY_BYTES` (`:350-358`, rejects
  the stream past the cap via `h3_reject_request_stream`). On-reactor but
  bounded — a correctly-classified non-issue. (An earlier automated pass
  called this "unbounded"; it is not.)

### 7. Static inline response — bounded by inline threshold

- `http_static_try_serve` HANDLED path populates `response_zv` synchronously in
  `http3_stream_dispatch` (`src/http3/http3_dispatch.c:218-230`) for small
  inline files / 4xx. Bounded by the inline-size threshold; larger files take
  the async pump (#9). On-reactor, bounded.

## Coroutine-safe sites (yield correctly)

- **User handler coroutine** — enqueued at `http3_dispatch.c:279`
  (`ZEND_ASYNC_ENQUEUE_COROUTINE`). Awaits DB/IO yield the reactor. Caveat: a
  CPU-bound handler with no await still monopolises the thread (see model
  note). This is the surface the CODING_STANDARDS rule guards.
- **Streaming `append_chunk`** — `src/http3/http3_callbacks.c` stream ops;
  parks on `ZEND_ASYNC_SUSPEND` when the flow-control window is full, so a slow
  client backpressures the producer instead of spinning the reactor.
- **sendFile / static pump** — `src/http3/http3_static_response.c`; 16 KiB
  async `ZEND_ASYNC_IO_READ` chunks, each a yield.

## Side observation (not fixed here — flagged for follow-up)

`http3_listener_poll_cb`: when `recvmmsg` returns a **full** batch
(`n == HTTP3_LISTENER_RECV_BATCH`) the outer loop iterates again; if the next
`recvmmsg` then returns `EAGAIN`, the function takes the early-exit that
**skips `http3_listener_flush_dirty`** (`http3_listener.c`, the `n <= 0`
branch). Any connections dirtied in the full batch therefore drain on the
*next* wakeup rather than this one — a sub-RTT response-latency hiccup, not a
correctness bug, and the dirty-list "empty between ticks" invariant still holds
across the gap. The Phase 0 watchdog instrumentation preserves this behaviour
exactly (the early-exit `goto tick_done` jumps past the flush). Worth a
deliberate decision later: flush before bailing, or leave it.

## What Phase 0 instrumentation now exposes

Per-listener, surfaced via `HttpServer::getStats()` (the H3 listener entry):

- `reactor_ticks`, `reactor_busy_ns`, `reactor_max_tick_ns` — tick volume +
  worst/total reactor occupancy.
- `reactor_slow_ticks` — ticks over the budget (`PHP_HTTP3_REACTOR_BUDGET_MS`,
  default 10 ms).
- `reactor_lat_bucket[12]` — tick-latency histogram with ACK-budget-aligned
  edges (bucket 8 is the first past `max_ack_delay` = 25 ms).
- `reactor_timer_late`, `reactor_max_timer_late_ns` — per-connection ACK/PTO
  service delay (timer fired this much past its armed deadline).

A rate-limited `WARN` (`h3.reactor.slow_tick …`) fires at most once/second when
a tick exceeds the budget.

**Reading them:** if `reactor_slow_ticks` and `reactor_max_tick_ns` climb under
a workload, sites #1 (buffered compression) and #2 (inbound decompress) are the
first suspects; correlate with response sizes / Content-Encoding. That is the
empirical signal that justifies starting Phase 1 (offload heavy ops) rather
than guessing.
