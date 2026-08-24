# TODO — HTTP Server Performance Backlog

## Step 1 — HTTP/3 hot path

Most of this is already done: handler coroutines do not suspend on write, UDP_SEGMENT / GSO batching is implemented in `src/http3/http3_listener.c:584` (`http3_listener_send_gso`), and Linux uses direct `sendmsg(MSG_DONTWAIT)` bypassing libuv.

Remaining:
- `MSG_ZEROCOPY` for large QUIC DATA frames (> 8 KB) — can be addressed together with Step 3

## Step 2 — Full TLS optimization (deferred)

When revisited:
- `setsockopt(TCP_ULP, "tls")` — kernel TLS offload (kTLS)
- `SSL_sendfile` for large responses
- Reduce memcpy between BIO ring buffers
- Switch to socket-BIO to eliminate the extra copy layer

## Step 3 — Zero-copy for large responses

**Goal**: avoid CPU cost of copying large response bodies into kernel on send.

- Threshold-based: apply only when `len > 16 KB` (page-pin overhead makes it harmful for small responses)
- Add a flag to `ZEND_ASYNC_IO_WRITE_EX` to request zero-copy mode
- `libuv_reactor.c`: direct `send(MSG_ZEROCOPY)` bypassing libuv, drain error queue via `recvmsg(MSG_ERRQUEUE)` to invoke `free_cb`
- `iouring_reactor.c`: `IORING_OP_SEND_ZC`

**Expected effect**: 10–30% CPU saving on large-body responses; more significant on NUMA under L3 cache bandwidth pressure.

## Step 4 — Brotli encoder reuse via custom arena allocator

**Problem.** gzip recycles encoder state between requests via `deflateReset()` — `gz_reset` is wired into `http_compression_pool.c`, hit rate ~99.999%. Brotli has no public reset API (`BrotliEncoderState` is opaque). Today `http_compression_brotli.c:br_create` allocates fresh state every request and `br_destroy` tears it down. Measured impact on `/json/40` (HttpArena `json-comp`):

| Accept-Encoding | RPS | latency p50 |
| --- | --- | --- |
| identity | 257k | 4.8 ms |
| gzip only (pool hit) | 103k | 6.8 ms |
| gzip, br (brotli picked) | **54k** | **51.8 ms** |

The 2× gap vs gzip is malloc-storm inside `BrotliEncoderCreateInstance`, not algorithmic — at q=4 brotli encode speed is roughly equivalent to gzip-6.

**Reference.** nginx (`google/ngx_brotli` and Cloudflare's fork) keeps brotli fast despite the same per-request `BrotliEncoderCreateInstance` call by pointing `alloc_func`/`free_func` at the per-request `r->pool`, which is itself reused across requests via nginx's pool freelist. The encoder is conceptually "recreated", but the underlying pages stay warm.

**Approach.**
1. Per-thread (`ZEND_TLS`) bump-pointer arena, ~1-2 MiB initial, growable.
2. Wire `BrotliEncoderCreateInstance(br_arena_alloc, br_arena_free, &tls_arena)` in `br_create`. `free` is a noop; `arena_reset()` happens on encoder release.
3. Implement `br_reset` in the brotli vtable so `http_compression_pool` caches the wrapper too. `br_reset` = `Destroy(state) + arena_reset + Create(state, arena_alloc, ...)` — the destroy/create roundtrip becomes free.
4. Same plumbing works for `BrotliDecoderCreateInstance` on the request side.

**Expected effect.** brotli RPS climbs from ~54k toward gzip's ~103k (or higher: better ratio means fewer TLS-encrypt cycles and fewer write syscalls per response). Removes the json-comp gap vs Swoole.

**Workaround already in place.** `http_compression_negotiate.c:174` flipped to prefer `gzip > brotli` when client expresses no q-value preference. Buys back ~2× RPS on the bench at the cost of slightly larger payloads on the wire. Real production clients that explicitly want brotli (`br;q=1.0, gzip;q=0.5`) still get it. Revert this flip once Step 4 lands.

**References.**
- libbrotli encoder API: `c/include/brotli/encode.h` — `BrotliEncoderCreateInstance(alloc_func, free_func, opaque)`
- ngx_brotli filter: `filter/ngx_http_brotli_filter_module.c:ngx_http_brotli_filter_ensure_stream_initialized`
- Discussion of missing reset: google/brotli#1132
## Step 5 — Worker recycle for RSS reclamation (`setMaxRequestsPerWorker(N)`)

**Problem.** Long-running workers under high-concurrency bursts grow their Zend MM commitment to a peak that is never returned to the OS. With Symfony-spawn-tas on the HttpArena leaderboard, `baseline-h2` at `c=1024 m=100` peaks at **5–17 GiB** committed PHP heap. The fight is not a real leak — it is Zend MM chunk retention, documented below.

### Where the bytes actually sit

Measured on the release build with `Async\runtime_stats()`, `HttpServer::getRuntimeStats()`, `memory_get_usage(true/false)` and a `gc_mem_caches()` probe; on a debug build with `zend_mm_dump_live_allocations()` (after `--enable-mm-php-source-track`) for PHP-source attribution.

After 8 × 50k-request bursts on the plain entry-handler (no Symfony):

| stage | user | mm chunks | RSS |
| --- | ---: | ---: | ---: |
| baseline (idle) | 0.49M | 2.00M | 33.9M |
| burst 1 + pause | 0.58M | **4.00M** | 39.5M |
| burst 2 + pause | 0.58M | 4.00M | 40.2M |
| burst 3–8 + pause | 0.58M | 4.00M | 40.5–40.8M |
| `gc_mem_caches()` | 0.58M | 4.00M | 40.8M (2.15M trimmed in free-lists) |

Reading: the first burst widens the chunk pool by one 2 MiB chunk; subsequent bursts **reuse it perfectly** — chunks plateau. RSS still creeps ~150 KiB per burst, which is glibc heap (libuv read/write buffers, nghttp2 internal state, OpenSSL session cache) — NOT Zend MM.

### Why MM does not return chunks to the OS

A chunk is 2 MiB, freed slots go into the per-thread free-list, the chunk itself is `munmap`'d only when it is **completely empty**. After a peak burst, persistent allocations (interned strings, opcache class entries, JIT runtime cache, Symfony container singletons, Doctrine proxies) are scattered across all committed chunks — one persistent slot per chunk pins the whole chunk down. `gc_mem_caches()` trims free-lists but cannot relocate the persistent residents, so chunks stay mapped.

```
chunk (2 MiB) = 512 pages × 4 KiB
██░░░░░░░░░░░░  ← persistent slot (10 KiB interned string)
░░░░██░░░░░░░░  ← persistent slot ( 8 KiB opcache class entry)
░░░░░░░░░██░░░  ← persistent slot
Live ~30 KiB out of 2048 KiB (1.5 % used). Chunk stays mapped. ZendMM does not relocate.
```

This matches FPM by design — FPM kills workers via `pm.max_requests` and gets RSS amnesia for free. We have no equivalent.

### Confirmed not a leak

- **ASAN/LSan clean** after 150k req + clean shutdown. Only false positives are OpenSSL `CRYPTO_malloc` globals (~3 KiB across 30 allocations).
- `coroutines_total` drops back to 2 (scheduler + acting handler) between bursts — no coroutine retention.
- `fiber_pool_count` ≤ 4 — fiber stacks are bounded by `ASYNC_FIBER_POOL_SIZE`.
- `conn_arena_live` matches active TCP connection count — no slot leak.
- `body_pool_total_bytes` stays at 0 unless the workload exercises ≥1 MiB request bodies.

### What Step 5 ships

`HttpServerConfig::setMaxRequestsPerWorker(int $n)`: after `$n` dispatched requests a worker drains its in-flight set, returns from `start_thread`, and the pool spawns a replacement. End of the worker process means `munmap` on every Zend MM chunk it ever allocated — clean amnesia, FPM-style.

Notes for implementation:
- Recycle must be cooperative — drain current chunk queue / streaming bodies before exiting, otherwise mid-flight responses get aborted.
- Pool-level overlap: spawn replacement before retiring the current worker so accept queue does not stall.
- A jittered cap (e.g. `n ± 10 %`) per worker so the whole pool does not recycle in lockstep.
- Disabled by default. Recommended values: `0` (off) for tests; `100k–500k` for production benches; `1M+` for normal workloads where the chunk plateau is acceptable.

### Adjacent observability work (already shipped)

- `Async\runtime_stats()` — coroutine/fiber/queue/microtask counters [php-async@main].
- `HttpServer::getRuntimeStats()` — `conn_arena` + `body_pool` counters.
- `zend_mm_dump_live_allocations()` (debug build) — live emallocs grouped by `(c_file, c_line, orig, php_file, php_line)`. `php_file:php_line` populated when configured with `--enable-mm-php-source-track` [php-src@true-async-stable].

## Step 6 — HTTP/2 TLS emit path: verified findings

Audit of the H2-over-TLS emit path (`http2_strategy.c` / `http_connection_tls.c`), each confirmed by reading the code. Listed by real severity, not by how bad they sound.

### 6a — TLS write deadlock when body > cipher ring (CORRECTNESS — fix first)

Tracked as **issue #29**. When the response body exceeds the CT-out BIO ring (`TLS_BIO_RING_SIZE = 64 KiB`), the drain-wake can fail to reach the writer and the connection hangs. Progress depends entirely on `tls_cipher_completion` re-entering `tls_drain`; in the overflow path that wake does not arrive. The 64 KiB ring is deliberately kept large to avoid triggering this — **do not shrink it until fixed**. This is the only real bug in this list.

### 6b — `tls_space_event` is a broadcast trigger, not per-waiter (perf, low severity)

`tls_space_event` is a `zend_async_trigger_event_t` (`http_connection.h:128`). When ring space frees, `trigger()` wakes **every** coroutine parked in `tls_wait_space` (`http_connection_tls.c:87`); each then re-checks `BIO_ctrl_get_write_guarantee`. With N streams blocked on a full ring this is O(N) wakeups where only 1–2 can proceed — thundering herd. Only bites under high multiplexing + slow network + large bodies. Fix = redesign the emit pump onto a microtask / per-waiter wake (already flagged informally as "emit pump needs a microtask").

### 6c — GATHER stages small records through `stage[16 KiB]` with memcpy (perf, minor)

`h2_emit_flush_tls_records` (`http2_strategy.c:1228`) memcpy's records `< H2_TLS_RECORD_PAYLOAD_MAX (16384)` into a stack `stage[]` before one `SSL_write` (`:1259`). This copies exactly the small bodies the path should be cheap for (measured regression, commit 597a474). Mostly mitigated: hybrid mode (default) only enters GATHER when `large_streams_pending > 0`; small responses with no large stream in flight go through DRAIN (no staging). Touch only if profiling flags it.

### 6d — emit mode selected via env var, not a setter (style / consistency)

`h2_tls_emit_mode()` reads `getenv("TRUE_ASYNC_H2_TLS_EMIT_MODE")` (`http2_strategy.c:1144`), cached process-wide, cannot be set per-server. Violates the "tunables go on `HttpServerConfig` setters, not env/INI" convention. Not a perf or correctness issue — a debug knob to migrate onto a setter during cleanup.

### Explicitly NOT a problem (do not re-flag)

- **Single in-flight cipher write per connection** (`tls_cipher_inflight` gate, `http_connection_tls.c:141`). A TCP socket is a single ordered byte stream — concurrent writes are impossible regardless, and encryption is synchronous inside drain. Depth-1 socket write with completion-driven re-drain is the correct design; freshly produced bytes accumulate in the 64 KiB ring and ship on the next drain. Not a bottleneck.

## Audit 2026-08-22 — the three days of framing and ordering work

Read of `d85b090..ad2c0ec`, 97 non-merge commits: #197, #198, #200, #202, #204,
#206, #209, #211, #179. Duplication, SOLID with a named cost, and work added to
a per-request path. Nothing here is measured unless it says so.

### Duplicated logic

- ~~**High. `keep_content_length` is re-derived at five submit sites, and the
  copies disagree.**~~ Closed. The streaming sites read
  `http_response_keeps_declared_length`, which carries the same trailers rule the
  buffered path applies, and the buffered ones read
  `http_response_wire_content_length` (#235). The two answers still come from two
  functions, because a stream's count is the handler's own field in the table
  while the buffered path computes one and states it.
- **High. The HTTP/1 retirement verdict is computed twice** —
  `src/core/http_connection.c:3040` and `src/http1/http1_stream.c:118` — kept
  coherent by the `streaming` flag and a latch, with three comments to explain
  it. The counter conditions already differ. Fix: one
  `h1_decide_connection_close(conn, response_obj)` called from both.
- **Medium. The HTTP/3 reset sequence is written twice** and has drifted:
  `src/http3/http3_callbacks.c:1428` and `src/http3/http3_dispatch.c:475`. The
  reactor copy learned to drain before latching; the other did not.
- **Medium. Two park loops with opposite failure policy in one file.**
  `http_connection_outbound_wait_drain` (`src/core/http_connection.c:1706`)
  returns false when the waker cannot be allocated; `out_wait_for_tail` (`:1773`)
  returns true and submits — which under OOM is the overtake #211 exists to
  prevent.
- **Medium. The four-slot frame is assembled twice** in
  `h1_stream_append_chunk` (`src/http1/http1_stream.c:317` and `:352`).
- **Medium. The non-blocking entry points spell the same refusal protocol three
  times** (`src/http_response.c:1498`, `:1810`, `src/http_sse.c:442`, with the
  tail at `:1536` and `:1834`).
- **Low.** `grpc_message_begin_stream` (`src/http_response.c:1734`) repeats the
  three-flag commit of `http_response_stream_commit_once` without saying why it
  may not share it. RFC 9110 §8.6's forbid-list is spelled at
  `src/http1/http1_format.c:161` and again inline at
  `src/http_response_server_api.c:499`. The HEAD drop-and-record block is copied
  four times (`src/http_response.c:1428, 1506, 1762, 1818`) and a fifth site
  drops without recording (`src/http_sse.c:250`).

### SOLID, where the cost is named

- **Medium, and a defect rather than debt.** `h1_response_state_connection`
  promises to set no field on HTTP/2 or HTTP/3 (`include/http1/http1_stream.h:64`)
  and tests `protocol_version == "1.1"` instead (`src/http1/http1_format.c:143`),
  so "2.0" and "3.0" read as an HTTP/1.0 peer and `Connection: keep-alive` is
  written into the table of every H2/H3 static-file response
  (`src/send_file.c:236, 372, 455`). Only the submit-time filter keeps it off
  the wire.
- **Medium, and a defect rather than debt.** `worker_stream_wait_writable`
  discards the caller's deadline (`src/core/worker_dispatch.c:499`), which the
  vtable slot defines as the caller's own bound: `awaitWritable(50)` on a pool
  worker parks for the configured write deadline.
- **Medium.** `h1_stream_append_chunk` chooses between four transport
  dispositions behind two `#if` fences (`src/http1/http1_stream.c:181-465`) — a
  decision http_connection already owns.
- **Low.** The compressing wrapper enforces the refusal precondition in prose
  (`src/compression/http_compression_response.c:571`); a transport that refuses
  without publishing `sendable` loses a flushed block per refusal, silently.

### Per-request cost

- **Medium.** `http_connection_send_strv_owned` emallocs an iov and an
  over-allocated slot block per fire-and-forget response
  (`src/core/http_connection.c:2145`), where before #209 it was one direct
  submit. A fixed per-connection slot block would carry both, since
  `out_in_flight` already serialises the writer. Not measured: h2load on a small
  keep-alive response against the commit before `5d5b40b` settles it.
- **Low/medium.** Every `setHeader`/`addHeader` lowercases the name twice
  (`src/http_response.c:289` then `:325`), and the server-field test runs for
  every header though only lengths 10 and 17 can match. Not measured.
- **Low.** Two fresh `zend_string`s per streamed chunk on the awaited plaintext
  path (`src/http1/http1_stream.c:352`). Not measured.
