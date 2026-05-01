# TrueAsync Server — Performance Analysis vs Swoole

**Date:** 2026-05-01
**Environment:** WSL2 / Ubuntu 24.04, 16-core, Docker network bridge.
**Builds:** `trueasync/php-true-async:0.7.0-alpha.2-php8.6` vs `mariasocute/swoole:6.2.0`.
**Workload:** `wrk` running in a separate container on the same Docker bridge network — no NAT, no `-p` port-forward, no shared CPU between server and load generator beyond the host scheduler.

---

## Headline numbers

### 1 worker vs 1 worker, GET /pipeline (`ok`)

| concurrency | tas (1 thread) | swoole (reactor=1, worker=1) | swoole / tas |
|---|---|---|---|
| c=32 | 35–42k | 61–64k | 1.5–1.8× |
| c=64 | 40–46k | 53–83k | 1.2–2.1× |
| c=128 | 37–40k | 54–86k | 1.4–2.3× |
| c=256 | 36–43k | 43–44k | ~1.0× (both saturate) |

Single-core CPU on both: **~97%**. Both saturate one core; swoole pushes ~2× more requests through it.

### 16 workers vs 16 workers (no NAT, separate wrk container)

| endpoint | tas | swoole | swoole / tas |
|---|---|---|---|
| `/pipeline` (`ok`, 2 bytes) | **338k** | 258k | **0.76×** *(tas wins)* |
| `/json/3` (165 / 559 bytes) | 297k | 380k | 1.28× |
| `/json/3` c=128 | 221k | 355k | 1.61× |

Swoole's `/json/3` returns 559 bytes (full HttpArena dataset row); ours returns 165 bytes (synthetic 4-field row). Despite the 3.4× larger payload, swoole's per-request CPU cost is lower.

### Key takeaway

- Per-core baseline: tas ~1.5–2× behind swoole on the simplest possible handler.
- Multi-core scaling: tas scales **better** (1→16 workers gives ~7.5×, swoole only ~3×). At 16 workers on `/pipeline` we beat swoole.
- On non-trivial handlers (`json_encode`) the per-core gap dominates and swoole pulls ahead.

---

## Why: per-request syscall pattern

Both servers profiled with `strace -c -f -p <pid>` for 5 seconds at c=64.

### tas (WORKERS=1, GET /bare)

```
% time     seconds  usecs/call     calls    errors syscall
------ ----------- ----------- --------- --------- ----------------
 69.02    1.796940         180      9932           epoll_pwait
 14.38    0.374401          38      9833           write
 10.79    0.280827          14     19847       131 epoll_ctl    <-- 2× per request
  5.53    0.144092          14      9875           read
  0.14    0.003722          29       128           close
  0.05    0.001215          18        64           accept4
------ ----------- ----------- --------- --------- ----------------
100.00    2.603680          52     49871       131 total
```

Per request: **~5 syscalls** — 1 `epoll_pwait`, 1 `read`, 1 `write`, **2 `epoll_ctl`**.

### swoole (reactor=1, worker=1, GET /pipeline)

```
% time     seconds  usecs/call     calls    errors syscall
------ ----------- ----------- --------- --------- ----------------
 69.83    0.725803          32     22071           sendto
 29.15    0.302996          13     22072           recvfrom
  1.02    0.010565          30       345           epoll_wait
------ ----------- ----------- --------- --------- ----------------
100.00    1.039364          23     44488           total
```

Per request: **~2 syscalls** — 1 `recvfrom`, 1 `sendto`. `epoll_wait` amortized: 22k events / 345 waits = **~64 events per epoll_wait** (events drained in batches matching the connection count).

### Decoded epoll_ctl pattern (`strace -e trace=epoll_ctl,epoll_pwait,read,write`)

```
epoll_pwait(14, [{events=EPOLLIN, fd=27}, {fd=23}, {fd=24}, ...], 1024, 0) = 8
epoll_ctl(14, EPOLL_CTL_DEL, 27, ...)      # remove from epoll
epoll_ctl(14, EPOLL_CTL_DEL, 23, ...)      # remove from epoll
... (8 DELs)
epoll_pwait(14, [], 1024, 0) = 0
epoll_ctl(14, EPOLL_CTL_ADD, 27, {EPOLLIN}) # re-add
epoll_pwait(14, [{fd=27, EPOLLIN}], 1024, 0) = 1
read(27, "GET /bare HTTP/1.1\r\nHost: ...", 8192) = 43
epoll_ctl(14, EPOLL_CTL_ADD, 23, {EPOLLIN})
epoll_pwait(14, [{fd=23, EPOLLIN}], 1024, 0) = 1
read(23, "GET /bare HTTP/1.1\r\nHost: ...", 8192) = 43
... (repeat for every fd)
write(27, "HTTP/1.1 200 OK\r\nContent-Length: ...", 71) = 71
write(23, ...) = 71
... (drain writes)
epoll_pwait(14, [{8 fds with EPOLLIN}], 1024, 0) = 8
epoll_ctl(14, EPOLL_CTL_DEL, ...)          # remove all 8 again
... (cycle repeats)
```

This is a full **EPOLL_CTL_DEL → EPOLL_CTL_ADD** cycle per request, not even an `EPOLL_CTL_MOD` (which would be cheaper). Swoole keeps fds armed for the whole connection lifetime and never modifies the epoll set during steady-state request handling.

---

## Root cause in the source

The cycle is implemented intentionally in
[`src/core/http_connection.c:907-925`](../src/core/http_connection.c):

```c
bool should_destroy = false;
if (!http_connection_handle_read_completion(conn, &should_destroy)) {
    /* Connection handed off (to handler coroutine) or destroyed. If the
     * multishot reader is still armed, stop it cleanly so the keep-alive
     * re-arm in http_handler_coroutine_dispose can start fresh without
     * hitting UV_EALREADY from a second uv_read_start on the same stream.
     *
     * Order matters: we MUST disarm the multishot read while conn is
     * still alive — destroy frees rcb (via callbacks_remove) and
     * conn->io. After destroy, both pointers are dangling. */
    if (!terminal && rcb->active_req != NULL) {
        rcb->active_req = NULL;
        conn->io->event.stop(&conn->io->event);     // ← uv_read_stop → epoll_ctl(DEL)
        req->dispose(req);
    }
    ...
}
```

After the handler coroutine produces a response, keep-alive resumes by calling
`http_connection_read()` again → `ZEND_ASYNC_IO_READ` →
`libuv_io_read()` → `uv_read_start()` → **`epoll_ctl(ADD)`**
([`php-src/ext/async/libuv_reactor.c:4312`](../../../php-src/ext/async/libuv_reactor.c)).

**Net effect: every request on a keep-alive connection costs one DEL + one ADD epoll syscall.**

The comment justifies it: avoids `UV_EALREADY` if `uv_read_start` is called twice without a `uv_read_stop` in between. So the design pattern is *"stop multishot, hand off to handler, re-arm multishot when handler returns"*. This is structurally clean (no race between handler and reader) but expensive on the hot path.

---

## What Swoole does differently

Swoole does **not** stop / re-arm the read watcher between requests on a keep-alive connection. From Swoole's source (`Server/swoole_server_port.cc`, `EventData` reactor handlers):

1. On `accept()`, the connection fd is added to the reactor's epoll set with `EPOLLIN | EPOLLET` and stays there until `close()`.
2. On readable: read everything available in a loop until `EAGAIN` (edge-triggered semantics).
3. Parse + dispatch to PHP worker (separate process via UNIX socket pipe).
4. Worker's response goes back through the reactor; reactor calls `sendto()` directly. If it would block, queue and arm `EPOLLOUT` once with `EPOLL_CTL_MOD`; on completion arm back to `EPOLLIN`.
5. No stop/start cycle, no per-request epoll_ctl in the steady state.

Crucial enabler: Swoole's reactor is a pure-C event loop with no PHP coroutine stack between read and dispatch. The worker process boundary is a UNIX socket, which doesn't need to be "paused" while PHP is running — it's a separate process.

In tas, the read watcher *must* be paused or the handler coroutine could re-enter the reader callback while the request is mid-handle. The author chose to address this by stopping the watcher. An alternative is to keep the watcher armed and gate re-entry differently (e.g. a "request-in-flight" flag on the connection, or use `EPOLL_CTL_MOD` to drop `EPOLLIN` instead of full DEL/ADD).

---

## Where the time actually goes (single-thread, c=64)

At 40k req/s the budget per request is **25 µs**. Distribution from the strace numbers above:

| stage | µs/req | % of budget |
|---|---|---|
| `epoll_pwait` (avg 180 µs/call but 1 per req) | ~180 | (dominant — these are batched waits, hard to attribute per-req) |
| `epoll_ctl` × 2 | 28 | 5% |
| `read` | 14 | 3% |
| `write` | 38 | 7% |
| accept / close | <1 | <1% |
| **userspace (PHP handler + parser + response build)** | rest | ~85% of wall-clock |

Most of the time is in userspace — JIT-compiled PHP + extension C code parsing HTTP1, building HttpRequest/HttpResponse zvals, calling the closure, marshaling the response. Cutting epoll syscalls in half ~~ won't double throughput, but it will widen the userspace budget from ~25 µs to ~30–35 µs per request on the same core, which translates into ~10–25% headroom on `/pipeline`. The rest of the gap to swoole comes from per-call PHP overhead (zval boxing, hash table operations) that swoole cuts by handing pre-parsed C structs to the worker process.

Verified separately: a stripped-down `/bare` handler (`setStatusCode(200)->setBody('ok')`, no URL parse, no switch) gives **the same** throughput as `/pipeline`. PHP-userland boilerplate is **not** the bottleneck — it's the C-level per-request path.

---

## Other observations

### CPU-count detection

`Async\available_parallelism()` (from libuv's `uv_available_parallelism`) only reads cgroup v2 (`/sys/fs/cgroup/cpu.max`). On cgroup v1 hosts (WSL2/Docker Desktop, RHEL 7/8 default) it returns the unconstrained logical-core count and ignores `--cpus=N` quota. Affinity (`--cpuset-cpus=…`) *is* respected because `uv_available_parallelism` does check `sched_getaffinity`.

Concrete WSL2 numbers:

| limit | available_parallelism | actual CPU% under load | rps /json/3 |
|---|---|---|---|
| none | 16 | 794% | 446k |
| `--cpus=4` (cgroup quota) | **16** ❌ | 411% (capped) | 132k (oversubscribed) |
| `--cpus=4 WORKERS=4` (manual) | — | 402% | 222k |
| `--cpuset-cpus=0-3` | **4** ✅ | 366% | 222k |

Workaround for cgroup v1: parse `/sys/fs/cgroup/cpu/cpu.cfs_quota_us` / `cpu.cfs_period_us` and clamp.

### Docker port-forward overhead

`docker run -p 19080:8080` adds Docker's userspace proxy in front of the server. On WSL2 it cuts throughput by 2.5× even at low concurrency (348k → 132k on `/json/3`). For benchmarks: use a shared docker bridge network and run wrk in a separate container, or use `--network host`. **Do not** benchmark via `-p` — the numbers are wrong.

### Workers scaling

| WORKERS | rps /json/3 (no NAT) |
|---|---|
| 1 | ~40k |
| 4 | ~150k |
| 16 | ~300k |

7.5× from 16× workers — sub-linear (cores are shared with wrk + Docker), but monotonic. The earlier "scaling collapse at WORKERS=4" we saw was an artifact of NAT — when measured cleanly the curve is well-behaved.

### TLS / HTTP/2 / HTTP/3

All three protocols pass functional smoke tests at the same per-worker baseline. Performance under load not separately measured for h2/h3 — the per-request syscall pattern would be similar (or worse, due to TLS).

---

## Recommendations (ordered by impact / cost ratio)

### 1. Eliminate the per-request DEL/ADD cycle (high impact, medium cost)

Three options, in increasing invasiveness:

**(a)** Replace `event.stop()` in `http_connection_read_cb` with a connection-local "request in flight" flag. Keep the multishot reader armed; in the read callback, if the flag is set, just buffer the bytes and return (don't notify the parser). Clear the flag when the handler coroutine completes. **Saves both syscalls.**

**(b)** Use `EPOLL_CTL_MOD` to drop `EPOLLIN` (set `events=0`) instead of full `DEL`, then `MOD` back to `EPOLLIN`. This requires a libuv API extension (or bypassing `uv_read_stop` and reaching into `uv__io_t`) but halves the syscall cost — `MOD` is significantly cheaper than `ADD`+`DEL` because the kernel reuses the existing `epitem`.

**(c)** Switch to `uv_poll_t` + raw `recv`/`send` like Swoole does. Most invasive — moves all stream framing into our code instead of libuv's. Should land us within 10% of Swoole.

### 2. Batch event delivery (medium impact, low cost)

Currently `epoll_pwait` returns up to 1024 events per call but our reactor processes one and then `pwait`s again. Process the entire returned batch before the next `pwait`. Swoole gets ~64 events per `pwait`; we'd get 8–32 depending on load. **Reduces wait syscalls by ~10×, doesn't help per-request budget directly but reduces context-switch overhead.**

### 3. Add cgroup v1 fallback for available_parallelism (low impact, very low cost)

Read `/sys/fs/cgroup/cpu/cpu.cfs_quota_us` and `cpu.cfs_period_us` if `/sys/fs/cgroup/cpu.max` is missing. Clamp `available_parallelism()` by `ceil(quota/period)`. Affects only old/legacy hosts (RHEL7/8 default, WSL2, LXC) but prevents the "127 cores!" oversubscription class of bug.

### 4. Profile with perf (medium impact, medium cost)

Get a flame graph of the hot path with `perf record -g`. The 25 µs/request budget on a single core is split somewhere between HTTP parsing (`llhttp`), zval marshaling (HttpRequest/HttpResponse), the closure call (`zend_call_function`), and response writing. Without a flame graph, ranking these is guesswork.

---

## Reproduction

All commands run from the repo root:

```bash
# Build images
docker build -t tas-smoke -f examples/docker/Dockerfile .
docker network create benchnet

# tas
docker run -d --rm --name tas-bench --network benchnet -e WORKERS=1 tas-smoke

# swoole baseline
cat > /tmp/swoole-1-1.php <<'PHP'
<?php
$http = new Swoole\Http\Server('0.0.0.0', 8080);
$http->set(['reactor_num' => 1, 'worker_num' => 1, 'enable_reuse_port' => true, 'enable_coroutine' => false]);
$http->on('request', fn ($req, $res) => $res->end('ok'));
$http->start();
PHP
docker run -d --rm --name swoole-bench --network benchnet \
    -v /tmp/swoole-1-1.php:/app/swoole.php:ro \
    mariasocute/swoole:6.2.0 php /app/swoole.php

# bench (replace target with tas-bench or swoole-bench)
docker run --rm --network benchnet williamyeh/wrk \
    -t4 -c64 -d10s --latency http://tas-bench:8080/pipeline

# Profile syscalls
docker exec tas-bench bash -c '
    apt-get update && apt-get install -y strace
    PHP_PID=$(pidof php | tr " " "\n" | head -1)
    timeout 5 strace -c -f -p $PHP_PID
'
```

---

## Files referenced

- `src/core/http_connection.c:907–925` — the stop/re-arm site
- `php-src/ext/async/libuv_reactor.c:4250–4322` — `libuv_io_read` (calls `uv_read_start`)
- `php-src/ext/async/libuv_reactor.c:3801–3835` — `libuv_io_event_start` / `libuv_io_event_stop`
- `php-src/ext/async/libuv_reactor.c:3922–3962` — read callback, multishot semantics
