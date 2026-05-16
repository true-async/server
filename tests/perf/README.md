# Single-Worker Performance Tests

Reproducible single-thread (`WORKERS=1`) micro-benchmarks for the
`true_async_server` extension. Designed for **change-over-change**
comparison (e.g. before/after a patch on the same machine) rather than
absolute capacity numbers — those depend heavily on CPU, kernel, libuv
backend and TLS hardware acceleration.

The suite covers every code path that matters for a typical HTTP
workload:

| family    | exercises                                          |
|-----------|----------------------------------------------------|
| `setbody` | buffered `Response->setBody()` — REST API hot path |
| `static`  | `StaticHandler` — file delivery via async I/O      |
| `stream`  | `Response->write()` — chunked emission, ring queue |
| `upload`  | `POST` body parsing + buffering                    |

Each family runs against three transports:

| protocol | listener                          | driver  |
|----------|-----------------------------------|---------|
| `h1`     | plain HTTP/1.1                    | `wrk`   |
| `h2c`    | HTTP/2 plaintext (prior-knowledge) | `h2load` |
| `h2tls`  | HTTP/2 over TLS via ALPN          | `h2load` |

## Requirements

- `php` CLI built with the extension (release/ZTS recommended — debug
  builds give 2-3× lower numbers and skew comparisons; see project
  memory `bench-release-only`).
- `h2load` from nghttp2 (`apt install nghttp2-client`).
- `wrk` for HTTP/1.1 benches (`apt install wrk`). If absent, h1 rows
  are skipped automatically.
- A kernel that lets the perf-server bind ports `19100-19132`.

## Quick start

```bash
# full sweep — takes a few minutes
./tests/perf/run.sh

# just one family
./tests/perf/run.sh setbody

# one family + one protocol
./tests/perf/run.sh setbody h2tls

# faster sanity check
DURATION=2 WARMUPS=1 ./tests/perf/run.sh static h2c
```

## Knobs

| variable   | default                       | meaning                                |
|------------|-------------------------------|----------------------------------------|
| `PHP`      | `php`                         | PHP CLI binary                         |
| `EXT_DIR`  | `<project>/modules`           | directory containing `true_async_server.so` |
| `DURATION` | `6`                           | per-scenario wall time, seconds        |
| `WARMUPS`  | `2`                           | warmup runs before measurement         |

## Output

A pipe-aligned table per scenario row:

```
FAMILY   PROTO  SCENARIO     CONC        RPS        MB/s   BODY-SIZE
setbody  h2c    b3           c=10  m=32  338215.50  0.99   3B
setbody  h2c    b1k          c=10  m=32  315340.00  308.7  1024B
setbody  h2c    b16k         c=10  m=32  90565.40   1414.0 16384B
...
static   h2c    medium.bin   c=10  m=32  10053.50   2455.0 262144B
static   h2tls  medium.bin   c=10  m=32  5320.10    1297.7 262144B
```

Save the table before and after a change, diff RPS column.

## What each family measures

### setbody — `servers/server_setbody.php`

Routes: `/b3`, `/b1k`, `/b16k`, `/b64k`, `/b256k`, `/b1m`.

The handler returns a pre-built body string of the named size. Useful
for measuring the cost of **the framework itself** — request parsing,
response serialization, transport encoding (TLS / writev) — without I/O
in the path.

Concurrency profiles: `c=10 m=32` (low connection count, multiplex
heavy) and `c=100 m=10` (high connection count, less per-conn work).

> Note: `/b256k` and `/b1m` are skipped on `h2tls` because the buffered
> setBody path on TLS has a pre-existing bug stalling at the initial
> 64 KiB flow-control window. Tracked in project memory
> `bug-h2-tls-buffered-window-stuck`. The h2c and h1 transports cover
> the larger sizes.

### static — `servers/server_static.php`

Routes: `/static/tiny.txt` (256 B), `/static/small.html` (16 KiB),
`/static/medium.bin` (256 KiB), `/static/large.bin` (8 MiB).

Files are generated under `<tmpdir>/true_async_perf_static/` on first
run. Exercises `StaticHandler` end-to-end: open-file cache, `pread`
through `ZEND_ASYNC_IO`, chunk-queue assembly, HTTP/2 NO_COPY frame
emission, TLS encrypt path. Hits the libuv thread pool (or io_uring
when enabled) for file reads.

### stream — `servers/server_stream.php`

Route: `/stream/<size>/<chunk>` — e.g. `/stream/1m/16k` writes one
1 MiB response in 64 chunks of 16 KiB each via `Response->write()`.

Exercises the bounded chunk ring + backpressure path. The handler
suspends on a full ring (HTTP/2 `WINDOW_UPDATE` waits) and resumes from
write completion. Useful for measuring coroutine wake-up cost and the
emit-pump scheduling.

### upload — `servers/server_upload.php`

Route: `POST /` with a body of fixed size, echoes `OK\n`.

Drives the inbound parser, request body buffering, and HTTP/2 DATA
frame ingest. Useful when changing the request-side handling rather
than the response-side path.

## Adding a new scenario

1. Drop a new server script into `servers/` following the
   `_common.php` pattern (it sets up listener mode + TLS certs).
2. Add a `run_<name>()` function to `run.sh` plus a `with_server`
   line in the main sweep.
3. Pick a base port in `BASE_PORT` (offsets +0/+1/+2 are reserved for
   h1/h2c/h2tls).

## Comparing runs

```bash
./tests/perf/run.sh > before.txt
# apply patch, rebuild
./tests/perf/run.sh > after.txt
diff -y before.txt after.txt | less
```

For statistical confidence on the noisy ranges (3 B requests at c=100
typically dance ±10%), increase `WARMUPS=3 DURATION=10` and run the
script three times — take the median.

## TLS certificate

`certs/cert.pem` and `certs/key.pem` are self-signed dev certs
(copied from `tests/bench/tmp-bench/`). h2load skips verification via
its built-in behaviour; curl needs `-k`. Do **not** ship these.

## What the suite intentionally does *not* measure

- **HTTP/3 / QUIC** — has its own bench harness under `tests/bench/`.
- **WebSocket** — exercise via the wslay-based handler separately.
- **Multi-worker scaling** — single worker on purpose; multi-worker
  tests live elsewhere because they require workload distribution
  that is fundamentally different to micro-bench.
- **Latency percentiles** — h2load prints them on its own when run
  manually; the script extracts only RPS / throughput for the
  comparison table.
