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