# HTTP/3 benchmarking — tooling, method, and known traps

Hard-won facts from issue #59. Read this before benchmarking H3 so the same
hours are not spent rediscovering them.

## TL;DR

- The system `h2load` and `curl` are built **without QUIC** — useless for H3.
  Use the QUIC-enabled `h2load` we built (see below).
- **Always run `h2load` with `-t` equal to `-c`.** With `-t 1` and several
  connections, h2load starves connections in its single event loop and you get
  spurious multi-second p99 stalls that look like a server bug but are not.
- For a deterministic Phase-1-style coalescing measurement use
  `-c 1 -m 32` (one connection, many streams) — every datagram in a tick
  belongs to the one connection, so flush coalescing is maximally exercised and
  there is no multi-connection client noise.
- `sendmsg`-per-request is the coalescing metric: count it with `strace -c` on
  the server (client-independent). `PKTS_PER_REQ` from `getHttp3Stats()` counts
  QUIC datagrams, **not** syscalls — GSO collapses many datagrams into one
  `sendmsg`, so it is not a substitute.

## QUIC-enabled h2load

The stock `/usr/bin/h2load` (nghttp2 1.59) and `curl 8.12` link no ngtcp2 —
their `--alpn-list=h3` runs fail instantly. We built one:

- Binary (libtool wrapper, run this one): `/home/edmond/nghttp2/src/h2load`
- Do **not** copy the bare `.libs/h2load` elsewhere — it then links the system
  `libnghttp2` (1.59) and dies with `undefined symbol:
  nghttp2_session_callbacks_set_send_callback2`. The wrapper points at the
  in-tree 1.70 lib.
- Built from nghttp2 1.70 against `/usr/local` ngtcp2 1.22 / nghttp3 1.15 /
  OpenSSL 3.5 (the same QUIC stack our server links), `crypto_ossl` backend.
- Needs **g++-14**: nghttp2 1.70 uses C++23 `std::print`/`std::expected`;
  g++-13's libstdc++ has no `<print>`. Build the apps with
  `make -C src h2load CXX=g++-14 CXXFLAGS="-std=c++23 -O2 -g"`.
- `third-party/urlparse` is a submodule; clone it manually if `--depth 1`
  skipped it: `git clone https://github.com/ngtcp2/urlparse third-party/urlparse`.

Smoke: `/home/edmond/nghttp2/src/h2load --alpn-list=h3 -n 20 -c 2 https://127.0.0.1:PORT/`
→ expect `Application protocol: h3`, all `succeeded`.

`h2load` reports p50/p99 directly in the `request :` line (columns:
min max **median(p50)** p95 **p99** mean sd) and req/s in the `finished in …`
line. It updates QUIC flow control, so it transfers 1 MB bodies (h3client does
not — see below).

## Other tools

- `tests/h3client` — correctness client only: one in-flight stream per
  connection, no flow-control updates. Fine for phpt e2e and a smoke RPS, but
  it **cannot** measure Phase-1 coalescing (nothing to coalesce at 1 stream)
  and **stalls on bodies ≥ ~1 MB** (no stream flow-control credit). Not a perf
  load generator.
- `perf` — the `/usr/bin/perf` wrapper cannot find a binary for the WSL kernel
  (`5.15…-microsoft`). Use the versioned one that works:
  `/usr/lib/linux-tools-6.8.0-117/perf`.
  - SW/HW counters (task-clock, context-switches, cycles, instructions,
    page-faults) need `kernel.perf_event_paranoid` lowered:
    `sudo sysctl kernel.perf_event_paranoid=-1` (else only userspace `:u`).
  - Syscall **tracepoints** (`syscalls:sys_enter_sendmsg`) additionally need
    tracefs readable (`sudo mount -o remount,mode=755 /sys/kernel/tracing/`).
    Simpler: get syscall counts from `strace -c`, use perf for cycles/insns.
- `/usr/bin/time -v` (max RSS, page faults, ctx switches) and `ltrace -c`
  (malloc/free counts) are installed as lighter allocation/alloc-call probes.

## Measuring a server build: traps

- Run the standalone bench server `tests/bench/h3_bench_server.php` so the
  server is a separate process you can `strace`/`perf` and kill cleanly.
- **strace/perf teardown:** killing the tracer PID orphans the traced `php`
  child and hangs the `wait`. Kill the **tracee child first**
  (`pgrep -P <tracer>`), then the tracer flushes its `-c`/stat report and
  exits. The bench scripts do this.
- **Use a fresh UDP port per server start.** Reusing a port across rapid
  start/kill cycles races with the dying socket and produces flaky handshakes.
- **Median of N for the strace cell**, not a single run: a one-shot strace
  count is noisy (e.g. an early committed h3 baseline read 383 sendmsg from one
  run when the true value was ~426).
- Release PHP only: `/home/edmond/php-release/bin/php`.

## Findings (issue #59)

### Phase 1 — deferred / coalesced output: proven win

Deferring `drain_out` from per-inbound-datagram to once-per-tick (dirty-set,
nginx posted-push analog). A/B at `-c 1 -m 32`, 16 KB body, 4000 requests,
release build, cross-confirmed by strace + perf:

| metric | baseline | Phase 1 | Δ |
|---|---|---|---|
| sendmsg / request | 1.49 | 1.03 | **−31%** |
| RPS | 37 256 | 49 540 | **+33%** |
| cycles | 465 M | 386 M | **−17%** |
| CPU (task-clock) | 118 ms | 93 ms | **−21%** |
| instructions / req | 159 339 | 153 407 | −3.7% |
| context-switches, page-faults | — | — | unchanged |

At `-m 1` (one stream/conn, no coalescing opportunity) Phase 1 is roughly
neutral / a hair worse — measuring it there (e.g. with h3client) is what made
it *look* like a regression. Real multiplexed H3 traffic lives in the `-m > 1`
regime where it wins.

### The "c=10 multi-second p99 stall" is an h2load artifact, NOT a server bug

Symptom: at `-c 10` you intermittently see p99 of 2–4 s and RPS collapsing.
Root cause: **`h2load -t 1` drives all 10 connections from one event-loop
thread and periodically starves one**; its requests then park on the QUIC loss
timer (seconds). Proof: `-t 10` (one thread per connection) shows **no**
multi-second stalls (p99 ~7–9 ms across runs); `-t 1` is intermittent.

Not Phase 1, not a `zend_bailout`/exception killing coroutines (perf shows no
spin in our code), not RSA cost. The RSA routines dominating a stalled
profile (`ossl_rsaz_amm52x20…`, ~53%) are a **red herring** — they are just the
expected TLS-handshake signing cost; an ECDSA P-256 cert stalls identically, so
the signature algorithm is not the cause.

### Large bodies

1 MB responses work fine at low concurrency (`-c 1 -m 1 -n 10` → 10 MB, ~20 ms,
all succeed). They fail only with h3client (its flow-control limit) or under
`-m 32 × 1 MB` (320 MB in flight at once). Single-connection scales cleanly
`-m 1..32` → 7.7k → 51k RPS.

## Reproduce

```
# QUIC h2load smoke
/home/edmond/nghttp2/src/h2load --alpn-list=h3 -n 20 -c 2 https://127.0.0.1:PORT/

# deterministic Phase-1 metric (one build at a time, whatever is in modules/)
OUT=/tmp/h3.json tests/bench/h3_compare_h2load.sh

# A/B a code change: measure, git stash + make (baseline), measure, restore.
```
