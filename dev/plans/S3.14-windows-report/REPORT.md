# S3.14 on Windows: one AcceptEx against 32, 1, 2 and 4 workers

Measured 2026-11-12 on the machine below, for `dev/plans/S3.14-windows-task.md` as of
`263df72`. Every number here comes from the 70 runs listed in `tables.md`; nothing was
dropped.

## Machine

- Windows 11 Pro 26H2, 10.0.26300.9974.
- Intel Core i7-11700K @ 3.60 GHz, 8 cores, 16 logical; 32 GB RAM.
- Power plan: High performance (`8c5e7fda-e8bf-4a96-9a85-a6e23a8c635c`).
- Loopback, 127.0.0.1. TCP dynamic port range 49152 + 16384.
- Other load: desktop session with Chrome, Vivaldi, Docker Desktop, Claude Code.
  CPU over 5 s before the series: the busiest process (Chrome) used 1.14 CPU-seconds,
  everything else under 0.5 (`load-before.txt`). Nothing else was built or run during
  the series.

The 2026-09-30 entry names the same CPU on Windows 11 Pro 26200; this machine is on
26300.

## Builds

Fresh clones in `E:\s314`, built as `.github/scripts/windows/build_task.bat` builds
them, HTTP/3 left out (no `--enable-http3`, no `build_h3_deps.bat`):

| | commit |
|---|---|
| php-src `true-async` | `390ab2975a90b91411fab9f5c8bd7ea3a6e09509` |
| php-async `main` | `3499dd73b22012aa599269b02ed31dfc3f19e7e4` |
| true-async/server `bug/windows-test-gaps` | `263df720a08bf0c0207cc86b3b590ab9a65d2450` |
| php-sdk-binary-tools `php-sdk-2.7.1` | `c73faaf1cce914e2fc04da5587132f8f425996af` |
| vcpkg (fresh clone, as CI) | `177e8175121536d414f405ed8b3099b325a42638`: libuv 1.53.0, nghttp2 1.71.0 |

Toolchain: Visual Studio 2026 Build Tools, MSVC 14.51.36231, SDK runner
`phpsdk-vs18-x64.bat`; deps `phpsdk_deps --branch 8.6 --stability staging --crt vs18`.
Configure line: the CI one minus `--enable-http3`, i.e. `--disable-debug-pack
--without-analyzer --enable-async --enable-true-async-server --enable-http2
--enable-http-server-test-hooks --enable-tas-test-hooks --enable-sockets
--with-openssl=yes --enable-native-intrinsics=AVX2 --disable-test-ini`, Release ZTS,
`CFLAGS=/W3 /WX /wd4018 /wd4146 /wd4244 /wd4267`. Both extensions static.

One local step CI does not have: `ascii-build-system.ps1` strips the localized
(Cyrillic) OS caption from `PHP_BUILD_SYSTEM` in `main\config.w32.h`, which otherwise
fails `/WX` with C4828 on this machine.

- **one**: the three commits as they are.
- **many**: the same tree with this patch (`many.patch`):

```diff
@@ -7387,22 +7387,6 @@ static bool libuv_listen_start(zend_async_event_t *event)
 	async_listen_event_t *listen_event = (async_listen_event_t *) (event);
 
 	if (!listen_event->listening) {
-		if (!listen_event->is_unix) {
-			/* One AcceptEx at a time on Windows (a no-op elsewhere), set before
-			 * uv_listen. libuv keeps completed accepts in a LIFO stack, so a
-			 * paused listener holding several would hand the newest over first;
-			 * with one it holds at most one, and the rest wait in the kernel
-			 * backlog in arrival order. Worker threads sharing one socket also
-			 * get an even share this way. The cost is one accept per loop
-			 * iteration. */
-			const int error = uv_tcp_simultaneous_accepts(&listen_event->uv_handle.tcp, 0);
-
-			if (error < 0) {
-				async_throw_error("Failed to limit pending accepts: %s", uv_strerror(error));
-				return false;
-			}
-		}
-
 		const int error = uv_listen(
```

Checked: `php8ts.dll` of **one** contains the string `Failed to limit pending
accepts`, the one of **many** does not.

`php -n -v`, both builds (only the build time differs):

```
PHP 8.6.0-dev (cli) (built: Nov 12 2026 18:22:17) (ZTS Visual C++ 2026 x64 TrueAsync ABI v0.27.0)
Copyright © The PHP Group and Contributors
Zend Engine v4.6.0-dev, Copyright © Zend by Perforce
    with Zend OPcache v8.6.0-dev, Copyright ©, by Zend by Perforce
```

**many** was built at 18:22:16.

## Method

- Server `server.php`, run with `php.exe -n`: one listener on 127.0.0.1, backlog
  1024, `setWorkers(N)`, `setStatsEnabled(true)`, handler answers `ok`. **uncapped**:
  defaults; **capped**: `setMaxConnections(100000)`. CoDel is off in both: the script
  checks `getBackpressureTargetMs() === 0` and that `CODEL_TARGET_MS` is unset, and
  the harness removes that variable from the server's environment.
- The server reports ready once `getStats()['workers']` has N entries, plus 300 ms.
  After the burst the harness tells it how many responses the clients got; the
  parent polls `getStats()` until `totals.total_requests` reaches it, then writes each
  `workers[*].total_requests` and stops. Every worker entry had `total_requests`.
- Load: `client.py`, 5 processes of 200 non-blocking connects (10 for 2000), each
  armed and spinning on one go file; every connection sends
  `GET / HTTP/1.0\r\nHost: x\r\n\r\n` and reads to EOF. Time = latest EOF minus
  earliest first connect across the clients, on `time.perf_counter_ns()`.
  `clockcheck.py` confirmed that clock is shared by processes here (20 of 20 child
  readings fell between the parent's). The start spread between clients was
  0.05-0.64 ms.
- Client checked against a known server first: Python `http.server` on loopback,
  200 connects, 200 `HTTP/1.0 200 OK`; a closed port gives 5 of 5 connect errors
  10061, recorded.
- Order: 7 rounds; in each round, for N = 1, 2, 4: one/uncapped, many/uncapped,
  one/capped. A fresh server per run, a different port per run, 1 s between runs.
  Then 7 runs of one/uncapped/1 worker with 2000 connects.

Every run of the 70: 0 refused connects, 0 unfinished, all answers `HTTP/1.0 200 OK`,
server exit code 0, the workers' `total_requests` summed to the connect count.

## Results

Times in ms, min … max of 7 runs. Share = `total_requests` of each worker slot (the
slot's position in `getStats()['workers']`), min … max over the 7 runs.

### 1 worker, 1000 connects

| build | config | time, ms | share |
|---|---|---|---|
| one | uncapped | 569 … 1051 | 1000 |
| many | uncapped | 126 … 148 | 1000 |
| one | capped | 569 … 1075 | 1000 |

Both `one` cells are two clusters: one/uncapped 569, 572, 572, 574, 575, 577, **1051**;
one/capped 569, 573, 576, 578, 586, **1058**, **1075**.

### 2 workers, 1000 connects

| build | config | time, ms | w0 | w1 |
|---|---|---|---|---|
| one | uncapped | 94 … 111 | 509 … 544 | 456 … 491 |
| many | uncapped | 130 … 142 | 0 … 0 | 1000 … 1000 |
| one | capped | 98 … 107 | 507 … 557 | 443 … 493 |

### 4 workers, 1000 connects

| build | config | time, ms | w0 | w1 | w2 | w3 |
|---|---|---|---|---|---|---|
| one | uncapped | 86 … 99 | 152 … 427 | 1 … 397 | 114 … 393 | 6 … 282 |
| many | uncapped | 135 … 146 | 0 | 0 | 0 | 1000 |
| one | capped | 88 … 104 | 7 … 448 | 1 … 359 | 1 … 375 | 253 … 333 |

With `one` and 4 workers the split is not even: some worker got fewer than 40
requests in 4 of 7 uncapped runs (smallest shares 1, 1, 6, 15) and in 6 of 7 capped
runs (1, 1, 2, 7, 35, 37), and which slot it was changed from run to run. With `many`
the last slot took all 1000 in every run, at 2 and at 4 workers.

### 1 worker, 2000 connects, one / uncapped

| runs | time, ms |
|---|---|
| 7 | 1606, 1613, 1616, 1616, 1622, 1632, **2104** |

Every run of every cell, with its shares: `tables.md`.

## Against 2026-09-30

- one / uncapped / 1 worker / 1000: **does not match** 578 … 595. Six runs are 569 …
  577, a few ms below that range, and one is 1051, far above it. Nothing adjusted.
- one / uncapped / 1 worker / 2000: 1606 … 2104 against 1620 … 2127; the same shape,
  a fast cluster and one run about 0.5 s slower.
- many / 1 worker: 126 … 148, below the old "before" range 156 … 172.

The old entry does not give its libuv version or OS build beyond 26200, so the shift
is recorded, not explained.

## Odd or failed

1. **A first series aborted** on a harness bug: `run.py` read `ready` before
   `server.php` had finished writing it (`JSONDecodeError: Expecting value: line 1
   column 1`). The three control files (`ready`, `stats.json` and the harness's `done`)
   are written aside and renamed; the whole series was rerun. The 8 runs made
   before the abort are not counted in the tables (one/1w uncapped 578 ms, capped 573 ms; many/4w
   0/0/0/1000).
2. **`one` with 1 worker has a slow mode** 480 … 500 ms above the fast one: 3 of 14
   runs at 1000 connects, 1 of 7 at 2000. In those runs all five clients finish
   within 17 ms of each other (seq 2: 1058 … 1075 ms; seq 20 and 27: all equal), so
   the whole tail is late, not one client. Not seen with `many` or with 2 or 4
   workers. Cause not checked.
3. **`many` with several workers sends every accept to one worker**, the last slot,
   in 14 of 14 runs; the other workers stay at 0. This is what the comment in
   `tests/phpt/server/telemetry/012` says of Windows ("the worker started last").
4. **`stubs/HttpServerConfig.php` says the CoDel default is 5 ms**
   (`setBackpressureTargetMs`, "Default: 5"), while `src/http_server_config.c`
   has `DEFAULT_BACKPRESSURE_TARGET_MS 0` and `getBackpressureTargetMs()` returned 0
   here. Left as is: this task does not change the repositories.

## Not measured

The reload cell (4 workers, uncapped, 2000 connects, `$server->reload()` during the
burst) was added to the task in `7a9d25f`, after this series ran against `263df72`.
It was not run.

## Files

In this directory: `REPORT.md`, `tables.md` (every run with its shares, generated by
`summarize.py` from the raw results), and `scripts/`:

- the build: `deps.bat`, `build.bat`, `ascii-build-system.ps1`, run inside
  `phpsdk-vs18-x64.bat -t`;
- the measurement: `server.php`, `client.py`, `run.py`;
- checks and summary: `clockcheck.py`, `summarize.py`.

`run.py` expects the layout of the measuring machine: `scripts/` beside `obj-one/`
and `obj-many/` (the two object directories), with runs written to `runs/`. The raw
`results-*.jsonl`, the build logs and the per-run server logs stayed on that machine.

Rerun from the parent of `scripts/`: `python scripts\run.py main results-main.jsonl`,
`python scripts\run.py big results-big.jsonl`,
`python scripts\summarize.py results-*.jsonl`.
