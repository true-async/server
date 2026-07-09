# ISSUE: pool graceful-shutdown teardown hang (pre-existing, non-deterministic)

**Status:** RESOLVED (root cause found 2026-07-09). Not a server-repo bug — a
**stale PHP binary**. The fix already exists in php-async (ext/async) commit
`937bcfe`, 2026-07-04, shipped in TrueAsync ABI v0.24.0. The environment's
installed `/usr/local/bin/php` is ABI v0.23.0 built 2026-07-03 — one day
*before* the fix. Rebuild/reinstall PHP (≥ v0.24.0); no server code change.
**Discovered:** 2026-07-09, during review-follow-up work on the merged gRPC branch.
**Severity:** flaky CI hang — the process never exits, so a naive `run-tests`
invocation wedges until an external timeout kills it (and leaves orphan PHP
children that `timeout` on the runner does NOT reap).

---

## ROOT CAUSE (confirmed via gdb + A/B binary swap)

The earlier "leftover libuv handle in the server's completion-future path" lead
was **wrong**. `gdb -p <hung pid> -batch -ex "thread apply all bt"` on a live
hang shows:

- **Main thread** — `main → php_module_shutdown → libuv_reactor_quiesce →
  uv_cond_wait(child_thread_registry_cond)`. It is NOT in `uv_run`; it is blocked
  at module shutdown waiting for every child (worker) thread to deregister from
  `child_thread_registry` (`ext/async/libuv_reactor.c`).
- **The 3 worker threads** — each still alive in its own scheduler loop
  (`fiber_entry → libuv_reactor_execute → uv_run(UV_RUN_ONCE, timeout=200)`); one
  caught mid `zif_Async_delay` → the bootloader's `while(true){ \Async\delay(200); }`
  coroutine is **still running on every worker**.

Mechanism: `graceful_shutdown()` stops the *server* on each worker (listeners
closed, "worker exited" logged) but the persistent bootloader coroutine was
spawned into the worker's **main scope**, outside the server scope, so the
server-side scope drain never reaches it. A sync-mode (`coroutine_mode=false`)
pool worker only retires after `RUN_SCHEDULER_AFTER_MAIN` drains, and that drain
loops while any reactor handle is alive — the delay(200) timer never lets it
finish. The worker thread never ends → never deregisters → `libuv_reactor_quiesce`
(the process-exit join) blocks forever.

The fix (commit `937bcfe`) adds, on the worker's `done:` exit for `!coroutine_mode`
pools only, a `start_graceful_shutdown()` call *before* the drain — it cancels the
main-scope stragglers and arms the drain escape valve, so the worker retires.

### Proof (same server.so, same test, only the PHP binary differs)

| PHP binary | build | 056 repro |
|---|---|---|
| `/usr/local/bin/php` v0.23.0 | 2026-07-03 (pre-fix) | **hangs** (EXIT=124) every run |
| `php-src/sapi/cli/php` v0.24.0 | 2026-07-08 (has `937bcfe`) | **clean exit**, 5/5 runs |

### Resolution

Rebuild + reinstall PHP from the current tree (php-async ≥ v0.24.0) so the
runtime carries `937bcfe`. The server code (`http_server_class.c`) needs no
change for this hang. After reinstall, un-skip / re-enable the pool-shutdown
tests (055/056/057) in CI.

---

## Symptom

`tests/phpt/server/core/056-pool-shutdown-timeout-zero.phpt` (and the sibling
pool-shutdown tests `055-pool-graceful-shutdown`, `057-pool-reload-then-shutdown`)
**produce their full, correct output and then hang forever at process exit.**

Captured from a direct foreground run of the `--FILE--` body:

```
=== STDOUT ===
up=pong
stopped=clean            <-- start() returned, script main body finished
=== STDERR ===
[true-async-server] worker shutting down (reason=reload, grace=0s)
[true-async-server] worker exited
[true-async-server] worker shutting down (reason=reload, grace=0s)
[true-async-server] worker exited
[true-async-server] worker shutting down (reason=reload, grace=0s)
[true-async-server] worker exited
=== exit ===
EXIT=124   (timeout -k 3 60 fired; process did not exit on its own)
```

So the test **logic** completes: `graceful_shutdown()` runs, `start()` returns,
`stopped=clean` prints, and all 3 workers quiesce and log `worker exited`.
The hang is **after** that, during final teardown of the **main thread's**
scheduler. Something keeps the main libuv loop alive so `uv_run` never returns
and the process never reaches a clean exit.

## Key facts established

1. **Not caused by PR #109 (the review fixes).** Proven with a clean-tree repro:
   `git stash` all working changes → rebuild pristine merged `main` → run the
   same repro → **identical hang** (correct stdout, `EXIT=124`).
2. **Non-deterministic / flaky.** It does not hang every time:
   - `013-grpc-streaming-read` was observed as "Tests passed on retry attempt".
   - A full `core/` suite run once completed 107/108 (only a warn on retry).
   - So there is a race in final teardown, not a hard deadlock every run.
3. **The hang is a live libuv handle, not a TrueAsync deadlock.** If it were a
   scheduler deadlock, `resolve_deadlocks()` would fire; instead the loop has a
   legitimately-active handle and blocks in `epoll_wait` forever. Thread states
   observed via `/proc/<pid>/task/*/wchan`: main thread `futex_wait_queue`
   (joining), worker/reactor threads `do_epoll_wait`.
4. **The test shape that triggers it:** `setWorkers(3)` + `setShutdownTimeout(0)`
   + a **persistent bootloader coroutine** (`while(true){ \Async\delay(200); }`)
   that `graceful_shutdown()` is supposed to cancel. The `delay(200)` creates a
   recurring timer event on each worker; the test exists precisely to prove
   graceful_shutdown cancels it. The leftover live handle at MAIN-thread teardown
   is the thing to find.

## The right way to debug this (per the actual mechanism)

**"To understand the hang it's enough to know which events were not completed."**
Do NOT bisect blindly. The process is blocked in `uv_run` because ≥1 handle is
still active. Enumerate them:

- Add a `uv_walk(loop, print_cb, NULL)` / `uv_print_all_handles(loop, stderr)`
  dump at the point where the main scheduler is about to block during teardown
  (ext/async scheduler final loop), or gate it behind an env var.
- Cross-check with the TrueAsync scheduler's own leftover accounting:
  `ZEND_ASYNC_ACTIVE_EVENT_COUNT`, the `ASYNC_G(coroutines)` hash, and the
  `"%u microtasks were not executed"` warning path in
  `php-src/ext/async/scheduler.c`. In a DEBUG build these may already print —
  capture **all** stderr (earlier runs discarded it by grepping only for
  "Tests passed").
- The DEBUG PHP build here is `ZTS DEBUG TrueAsync ABI v0.23.0`.

Likely suspects to look at once the live handle is identified:
- the per-worker completion-future trigger events quiesced by
  `graceful_shutdown()` when `shutdown_timeout == 0` (the "stop trigger, unref,
  leave handle open for a safe late signal" path in `http_server_class.c`) —
  an unref'd-but-not-closed handle that still counts as active on the main loop.
- the bootloader persistent coroutine's timer if cancellation races worker exit.

## Reliable repro (safe — foreground, hard timeout, orphan cleanup)

`run-tests.php` masks this: it compares output, sees a match, reports
"Tests passed", then hangs reaping the child — and its child PHP survives a
`timeout` on the runner. **Always** run this class of test foreground with a
kill-timeout and pkill orphans afterward:

```bash
cd /home/edmond/true-async-server2
# extract the runnable body (relative require needs the test dir)
awk '/^--FILE--$/{f=1;next}/^--EXPECT/{f=0}f' \
  tests/phpt/server/core/056-pool-shutdown-timeout-zero.phpt \
  > tests/phpt/server/core/_t056_repro.php

timeout -k 3 60 php -n -d extension_dir=$PWD/modules \
  -d extension=true_async_server.so -d memory_limit=128M \
  tests/phpt/server/core/_t056_repro.php
echo "EXIT=$?"            # 124 == hung; 0 == exited cleanly this run (flaky)
pkill -9 -f "_t056_repro" # reap orphans regardless
rm -f tests/phpt/server/core/_t056_repro.php
```

Run it several times — it does not hang every invocation.

## Getting a stack (needs sudo — ptrace is restricted here)

`/proc/sys/kernel/yama/ptrace_scope == 1`, so `gdb -p` needs sudo:

```bash
sudo gdb -p <pid> -batch -ex "thread apply all bt 20"
```

Look for the main thread inside `uv_run` / the scheduler final loop, and check
which handle types are still active on that loop.

## Scope note

PR #109 (`fix/grpc-review-followups`) is the review follow-ups and is
independent of this hang. This teardown hang should be fixed on its own branch.
