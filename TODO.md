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
