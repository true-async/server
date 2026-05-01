# TrueAsync vs Swoole — Coroutine Overhead Analysis

**Date:** 2026-05-01
**Environment:** WSL2 + Docker, single PHP thread, Linux x86_64.
**Workload:** `wrk -t4 -c64 -d30s` against a minimal handler that emits `"ok"`.
**PHP build:** TrueAsync php-src (ZTS), Swoole 6.2.0 (NTS).
**Profiling tool:** `perf record -g -F 999 --call-graph dwarf -p <pid>` for 30 seconds.

## TL;DR

When both runtimes are compared with coroutines enabled — apples to apples — Swoole is **1.26× faster per thread** (54k vs 43k req/s). The dominant per-request cost on the TrueAsync side is the coroutine machinery itself: TrueAsync spends roughly **9.5%** of CPU on coroutine create/switch/destroy, Swoole spends **3.5–4%** for the same conceptual work. That ~6 percentage points alone accounts for the bulk of the per-thread gap.

This document quantifies the difference, shows where each cycle goes, and lists the concrete TrueAsync code paths that need to be tightened.

## Why this matters

Earlier analysis (`PERFORMANCE_ANALYSIS.md`) compared TrueAsync against Swoole with `enable_coroutine=false`, where Swoole completely skips its coroutine path. That comparison overstated the gap (1.51×) by attributing all of TrueAsync’s coroutine cost to a runtime that simply did not have one. The fair comparison is `enable_coroutine=true`, which mirrors the runtime model TrueAsync always uses.

## Methodology

Both servers run in Docker on the same host, on a shared bridge network. `wrk` runs in a third container on the same network, eliminating the docker `-p` userspace proxy overhead that distorted earlier numbers.

Handlers are reduced to a single PHP statement so that user-code cost is roughly the same on both sides:

| Server | Handler |
|---|---|
| TrueAsync | `fn ($req, $res) => $res->setBody('ok')` |
| Swoole | `fn ($req, $res) => $res->end('ok')` |

Server configs are minimal:
- TrueAsync: `WORKERS=1`, no TLS, no HTTP/3.
- Swoole: `reactor_num=1`, `worker_num=1`, `enable_reuse_port=true`, `enable_coroutine=true`.

`perf record` runs against the live PHP PID for 30 s under steady wrk load. Userspace-only sampling (no kernel symbol resolution) is enough — the gap lives in user code.

## Throughput

| Configuration | Req/s | p99 | Notes |
|---|---|---:|---|
| TrueAsync (always coroutine) | **43k** | 2.3 ms | This branch (`feat/multishot-alloc-cb`). |
| Swoole `enable_coroutine=true` | **54k** | 1.2 ms | Fair comparison. |
| Swoole `enable_coroutine=false` | 65k | 1.0 ms | Reference for "Swoole at maximum". |

Swoole loses 17% throughput when its own coroutines are turned on (65k → 54k). That is the direct cost of coroutine create/destroy on its hot path — and it is roughly the same shape as TrueAsync's, just much smaller in absolute size.

## CPU breakdown — coroutine machinery only

### TrueAsync (`async_*` + libuv reactor symbols)

| Self time | Symbol |
|---:|---|
| 2.67% | `jump_fcontext` |
| 2.63% | `async_scheduler_coroutine_suspend` |
| 2.51% | `fiber_entry` |
| 1.68% | `async_coroutine_execute` |
| **9.49% total** | |

### Swoole (`swoole::PHPCoroutine::*` + `swoole::Coroutine::*`)

| Self time | Symbol |
|---:|---|
| 0.47% | `swoole::PHPCoroutine::create_context` |
| 0.34% | `swoole::Coroutine::close` |
| 0.32% | `swoole::Coroutine::Coroutine` (ctor) |
| 0.30% | `swoole::PHPCoroutine::create` |
| 0.30% | `swoole::PHPCoroutine::main_func` |
| 0.27% | `swoole_jump_fcontext` |
| 0.23% | `swoole::PHPCoroutine::destroy_context` |
| 0.22% | `swoole::coroutine::Context::context_func` |
| 0.19% | `swoole::Coroutine::run` |
| 0.13% | `swoole::PHPCoroutine::on_close` |
| 0.13% | `swoole::PHPCoroutine::restore_context` |
| 0.13% | `swoole::coroutine::Context::Context` |
| 0.12% | `swoole::PHPCoroutine::fiber_context_try_destroy` |
| 0.12% | `swoole::PHPCoroutine::save_context` |
| **3.65% total** | |

### Ratio

TrueAsync spends **2.6× more CPU per request** on coroutine machinery than Swoole, despite both using Boost.Context (`jump_fcontext`/`swoole_jump_fcontext` are the same primitive). The actual context switch is the same handful of register saves; the cost difference is in everything around it — context allocation, scheduler bookkeeping, and per-coroutine state setup/teardown.

## Per-call breakdown (where the time goes)

A single request fires a small but predictable sequence of coroutine operations:

1. **Allocate a context** for the new request handler.
2. **Switch into it** from the reactor frame (`jump_fcontext`).
3. **Run user PHP** until completion.
4. **Switch back** to the reactor frame on completion or `await`.
5. **Tear the context down**, free the stack and any local state.

Mapped to the symbols above:

| Step | TrueAsync symbols (% self) | Swoole symbols (% self) |
|---|---|---|
| Allocate context | (folded into `async_coroutine_execute` 1.68%) | `PHPCoroutine::create_context` 0.47% + `Coroutine::Coroutine` 0.32% + `Context::Context` 0.13% ≈ **0.9%** |
| Switch in | `jump_fcontext` (half of 2.67%) ≈ **1.3%** | `swoole_jump_fcontext` (half of 0.27%) ≈ **0.13%** |
| Run user PHP | (covered by `execute_ex` 2.11%) | (covered by `execute_ex` 1.30%) |
| Switch out | `jump_fcontext` (other half) + `async_scheduler_coroutine_suspend` 2.63% ≈ **4.0%** | `swoole_jump_fcontext` (other half) + `Coroutine::close` 0.34% ≈ **0.5%** |
| Tear down | `fiber_entry` 2.51% (boilerplate on resume) | `PHPCoroutine::destroy_context` 0.23% + `Coroutine::close` part + `on_close` 0.13% ≈ **0.4%** |

The **switch-out + scheduler-suspend** path is the single biggest contributor on the TrueAsync side: ~4% of CPU per request goes into yielding back to the scheduler, even on a fully-synchronous handler that never needs to suspend.

## Why TrueAsync is heavier

A few patterns stand out from the call-graph data and source inspection.

### 1. Scheduler suspend runs even when there is nothing to suspend on

`async_scheduler_coroutine_suspend` accounts for 2.63% on the TrueAsync side. On a synchronous handler — the case our minimal benchmark hits — there is no `await`, no I/O wait, no timer, no other coroutine to schedule. Yet the scheduler is still entered, performs its bookkeeping, and returns. Swoole's equivalent (`Coroutine::close`/`PHPCoroutine::on_close`, ~0.5% combined) clearly fast-paths this case.

### 2. Boilerplate on every coroutine entry

`fiber_entry` at 2.51% is the constant per-coroutine setup cost: argument marshaling into the new frame, exception barrier, `try_first` flag, refcount adjustments. Swoole's `PHPCoroutine::main_func` covers the same conceptual work in 0.30% — roughly 8× cheaper.

### 3. Context allocation not pooled

Swoole reuses coroutine contexts across requests via internal slot recycling — `create_context` ends up calling into already-warm slabs. TrueAsync allocates a fresh context (and stack) on every dispatch, contributing to the 1.68% in `async_coroutine_execute` and to the broader memory-churn categories (`_emalloc` is 2.58% on our side vs 0.94% on Swoole's, though not all of that is coroutine-related).

### 4. No fast path for sync handlers

When a handler completes without ever calling `await`, both runtimes still pay the full coroutine create/switch/teardown cost. Swoole's machinery is just much cheaper, so the absolute cost stays small. TrueAsync has no equivalent of "skip the fiber switch when the handler returns synchronously" — every dispatch goes through `fiber_entry → async_coroutine_execute → user code → suspend → finalize`.

## Optimization opportunities

Listed by ROI on a synchronous-handler workload. Numbers are upper bounds on what is reachable if the named overhead is fully eliminated; in practice each will recover most of its budget but not all.

### A. Sync-handler fast path — up to **−5%**

The single biggest win. When the handler returns without ever awaiting:
- Skip `jump_fcontext` and the reactor return trip.
- Skip the scheduler-suspend entry.
- Run the handler in the reactor's own stack frame.

Implementation cost: medium. Requires a "did this coroutine ever yield" probe and a different finalization path. Should match Swoole's no-coroutine-mode performance for the synchronous case.

### B. Pool coroutine contexts — **−1 to −2%**

Maintain a freelist of recently-completed coroutine contexts (struct + stack). A new dispatch pops from the freelist instead of `pemalloc`. Swoole does effectively this; the win is visible in `_emalloc` and `create_context` deltas.

Implementation cost: low. Single freelist on the scheduler. Cap the size to bound memory.

### C. Slim down `async_scheduler_coroutine_suspend` — **−1%**

For the common case (no other ready coroutines, no expired timers), most of the function's bookkeeping is unnecessary. Add a fast path that detects "nothing to schedule" and returns immediately, skipping the heap traversal and event-list scan.

Implementation cost: low. Conditional on a single counter check.

### D. Inline `fiber_entry` boilerplate — **−1%**

Most of the entry code is the same on every dispatch (clear local state, set up argument frame, install exception barrier). Specialize a "small handler" entry for the common case where:
- The handler takes exactly 2 args (request, response).
- The handler returns void/null.
- No throw expected.

Implementation cost: low. Direct simplification of the entry trampoline.

### E. Stop crossing the scheduler when there is no work — already partially in scope

Several of the scheduler entries observed under load come from timer rearm and event-loop ticks that fire even when there is nothing to do. Reducing those (already partially addressed by the `alloc_cb` change in `feat/multishot-alloc-cb`) yields incremental wins.

## Combined estimate

If A through D land:

| Optimization | Savings |
|---|---:|
| A. Sync-handler fast path | up to 5% |
| B. Coroutine context pool | up to 2% |
| C. Scheduler-suspend fast path | up to 1% |
| D. Specialized `fiber_entry` | up to 1% |
| **Total** | **up to 9%** |

That would close most of the coroutine-overhead delta against Swoole (5.8 percentage points) and bring TrueAsync from **0.79× Swoole** per-thread to **~0.92–0.95×**, on a synchronous workload.

For workloads that legitimately suspend (database I/O, file reads, timers) the gap is intrinsically smaller because the suspend cost is amortized over real work — those benchmarks should be re-measured separately once the synchronous-path optimizations land.

## Reproduction

```bash
# TrueAsync (this branch)
docker run -d --rm --name tas-bench --network benchnet -e WORKERS=1 \
    tas-newopt php -d extension=/usr/local/lib/php/extensions/no-debug-zts-20250926/true_async_server.so \
                  /app/multi-worker.php

# Swoole — fair comparison
cat > /tmp/swoole-coro.php <<'PHP'
<?php
$http = new Swoole\Http\Server('0.0.0.0', 8080);
$http->set([
    'reactor_num'       => 1,
    'worker_num'        => 1,
    'enable_reuse_port' => true,
    'enable_coroutine'  => true,
]);
$http->on('request', function ($req, $res) { $res->end('ok'); });
$http->start();
PHP

docker run -d --rm --name swoole-bench --network benchnet --pid=host \
    --cap-add SYS_PTRACE --cap-add SYS_ADMIN --cap-add PERFMON \
    -v /tmp/swoole-coro.php:/app/swoole.php:ro \
    swoole-perf php /app/swoole.php

# Bench
docker run --rm --network benchnet williamyeh/wrk \
    -t4 -c64 -d10s --latency http://tas-bench:8080/

docker run --rm --network benchnet williamyeh/wrk \
    -t4 -c64 -d10s --latency http://swoole-bench:8080/

# Profile (run inside the respective container)
PHP_PID=$(pidof php | tr ' ' '\n' | head -1)
/usr/lib/linux-tools/6.8.0-110-generic/perf record -g -F 999 \
    --call-graph dwarf -p $PHP_PID -o /tmp/perf.data -- sleep 30
/usr/lib/linux-tools/6.8.0-110-generic/perf report -i /tmp/perf.data \
    --stdio --no-children --sort overhead,symbol --no-call-graph
```

## Files referenced

- TrueAsync coroutine path: `php-src/ext/async/scheduler.c`, `php-src/ext/async/libuv_reactor.c`, `php-src/Zend/zend_async_API.h`
- Swoole coroutine path: `swoole-src/src/coroutine/php_coroutine.cc`, `swoole-src/src/coroutine/context.cc`
- HTTP dispatch: `true-async-server/src/core/http_connection.c`
