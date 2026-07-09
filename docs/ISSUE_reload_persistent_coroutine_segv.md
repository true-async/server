# Known issue — SIGSEGV during pool reload when the bootloader spawns a long‑lived coroutine (#93)

Status: **RESOLVED (2026-07-09).** Both halves are fixed in php-async / TrueAsync
ABI **v0.24.0** and verified here after rebuild+reinstall:
- reload **deadlock** — fixed by `937bcfe` (sync-worker graceful-shutdown backstop).
- intermittent **SIGSEGV** (cross-thread run_time_cache UAF, #176) — fixed by
  `0cbdfe8` "deep-copy nested closures per worker" (PR #180). `thread.c` now
  `thread_persist_copy_xlat`'s `dynamic_func_defs` per worker, so nested closures
  are no longer shared across worker arenas.

**Verification:** the synthetic stress harness below (2 workers, persistent nested
closure `while(true){delay()}`, hot-reload rotation) ran **30/30 clean** on the
v0.24.0 build (`up=v1; after=v2; stopped=clean`, zero rc 139/134) — versus ~50%
SIGSEGV pre-fix. No server-repo change required; the environment just needed
php ≥ v0.24.0. Original investigation trail retained below for reference.

---

_Historical (pre-fix):_ the intermittent crash was **root-caused and filed as an
ext/async bug** [true-async/php-async#176](https://github.com/true-async/php-async/issues/176).

**Root cause (confirmed — code + ASAN + gdb):** `async_thread_create_closure`
(`thread.c`) does `memcpy(&func, copy->func, sizeof(zend_op_array))` — a *shallow*
copy of only the **top-level** op_array. Its `dynamic_func_defs` (nested closures
declared inside the transferred closure, e.g. the `spawn(fn: while(true) delay())`
inside the bootloader) are copied as a pointer → **all workers share the same
nested op_array** from the persistent snapshot (proven: same `func` address
`0x531…` on every worker; top-level closures are per-worker `0x7fff…`). Then
`zend_create_closure` writes a per-thread arena `run_time_cache` **direct pointer**
into that shared op_array (`zend_closures.c:817`), which is read from another
thread / after that worker's arena is freed → dangling read → SEGV. Fix belongs
in ext/async: deep-copy `dynamic_func_defs` per worker (like the top-level).
Everything below is the raw investigation trail.

---

## 1. Background: the original deadlock (fixed)

`HttpServer::reload()` under the built‑in worker pool (`mode=pool`, sync mode)
rotates the cohort: it bumps the epoch, each worker retires (drain #74 → stop →
`start()` returns), the ThreadPool `reload()` waits for every old worker to post
its exit token, then submits the fresh cohort.

A worker only posts its exit token after its scheduler has fully drained
(`ZEND_ASYNC_RUN_SCHEDULER_AFTER_MAIN` in `thread_pool_worker_handler`). That
drain loops while `has_handles` is true. If the **bootloader spawned a
never‑ending coroutine into the worker's main scope** — e.g. laravel‑spawn's
async DB pool arms a recurring `PDO::ATTR_POOL_HEALTHCHECK_INTERVAL` timer, or
any `Async\spawn(fn: while(true) …)` — that coroutine's live reactor handle keeps
`has_handles` true forever. Nothing cancels it on retire (drain #74 only touches
`server_scope`, not the main scope), so the drain never completes, the exit token
is never sent, and `ThreadPool::reload()` blocks forever. The user sees:

```
reload.start workers=8
[true-async-server] worker thread exited cleanly   ×8   (old cohort — normal)
<no reload.done, server stops serving>
```

Reproduced on Linux with the real laravel‑spawn app (the 8× "exited cleanly" is
the normal old cohort retiring; the bug is the missing `reload.done`).

## 2. Fix that shipped

- **ext/async `thread_pool.c`** — at the worker's `done:` exit, for sync‑mode
  pools only (`!pool->coroutine_mode`), call `start_graceful_shutdown()` before
  `RUN_SCHEDULER_AFTER_MAIN`. It cancels the main‑scope stragglers and arms the
  drain's escape valve, so the drain completes and the exit token is posted.
  Gated to sync mode so coroutine‑mode pools keep draining their in‑flight task
  coroutines (test `thread_pool/077`).
- **server `http_server_class.c`** — lifecycle logs so the journal reads
  `worker shutting down (reason=reload) → worker exited` instead of the
  confusing bare `worker thread exited cleanly ×N`.

Result: deadlock gone. **Real laravel‑spawn server: 10/10 reloads clean.**
Server hot‑reload phpt 5/5, ThreadPool suite 78/79 (077 green via the gate),
zero compiler warnings.

## 3. The remaining problem: intermittent SIGSEGV under stress

Under an aggressive synthetic harness (below), **~50 % of reloads SIGSEGV.** The
real server did not hit it in 10 reloads, but it is a real latent crash.

### Proven facts (gdb, release `-O2 -g` build)

Backtrace is stable:

```
#0 ZEND_INIT_FCALL_SPEC_CONST_HANDLER  Zend/zend_vm_execute.h:4186
       fbc = CACHED_PTR(opline->result.num);        // result.num = 0
#1 execute_ex
#2 zend_call_function                   Zend/zend_execute_API.c:1155
#3 async_coroutine_execute              ext/async/coroutine.c:532   // first start (SET_STARTED just above)
#4 fiber_entry                          ext/async/scheduler.c:1822
```

Inspecting the crashing coroutine:

- It is the **bootloader‑spawned persistent coroutine itself** — its closure is
  `{closure:{closure:…repro:25}:30}` (the `while(true){ delay() }`).
- The closure and its op_array are **VALID**: `fci_cache.function_handler` is a
  live `ZEND_USER_FUNCTION` (type 2), the closure object `refcount = 2`.
  **It is NOT freed** (an earlier "op_array freed" guess was wrong).
- Coroutine flags: `STARTED = 1` (set at coroutine.c:524 immediately before the
  crash — i.e. this is its **first** execution), `CANCELLED = 0`, `MAIN = 0`.
- The fault: `CACHED_PTR(0)` dereferences a **NULL run‑time cache**, even though
  `op_array.cache_size = 8`. So the op_array expects a run‑time cache but the
  `ZEND_MAP_PTR` resolves to NULL in this execution.

### What was ruled out

- **"Coroutine never ticked before reload"** — ruled out. A first‑run probe
  shows both workers' persistent coroutines run *during serving* (before the
  reload trigger). The crash happens later, in the rotation phase (a 3rd
  first‑run — the replacement worker re‑booting the bootloader — is observed
  after `reload.start`, then the crash).
- **Cancelled‑coroutine‑executed** — ruled out; the crashing coroutine has
  `CANCELLED = 0`.

### Failed fix attempt (do not repeat blindly)

Cancelling the main scope earlier, server‑side, in `start()`'s post‑wakeup path
(right after the #74 `server_scope` drain, on the still‑valid `start()`
coroutine, with the root coroutine `SET_PROTECTED`) made the crash **worse**
(~75 %). So the crash is **not** simply "the context is torn down by the time we
reach the pool `done:`": executing/cancelling this coroutine is unsafe at both
points. Reverted.

### Open questions for next session

1. Why does a **cloned/transferred closure** (created from the bootloader
   snapshot in the worker) have a **NULL run‑time cache** on the execution that
   crashes, while the *same* coroutine executed fine during serving?
2. Who **fresh‑starts** this coroutine during the rotation drain, and why is it a
   fresh start (not a resume) when it already ran during serving? Is there a
   second instance (replacement worker) vs the old instance being re‑entered?
3. Is the fix better placed in ext/async (make the drain skip/deallocate
   never‑resumable coroutines, or ensure the run‑time cache is bound before
   execution) rather than in the server?

## 4. Reproduction

Build the release stack (php‑src `true-async-stable` + ext/async `main` →
`/home/edmond/php-release`, then the server extension against it). Run the
synthetic harness — a trivial bootloader that leaves one persistent coroutine,
which is all it takes:

```php
// repro_persistent.php  (2 workers, watcher hot‑reload)
$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', (int)$argv[1])
    ->setWorkers(2)
    ->setLogSeverity(TrueAsync\LogSeverity::INFO)->setLogStream(STDERR)
    ->setBootloader(static function () use ($code) {
        define('APP_V', (string) include $code);
        Async\spawn(static function () {          // <-- the trigger: a never‑ending
            while (true) { Async\delay(1000); }   //     main‑scope coroutine
        });
    })
    ->enableHotReload([$dir], ['php'], 150, 1000);
// a spawned driver curls until v1, rewrites the watched file to v2, curls for v2.
```

Loop it on fresh ports; ~half the runs SIGSEGV (rc 139), the rest complete with
`reload.done` + `after=v2`. The crash needs the parent reactor busy (the driver's
blocking `shell_exec(curl)` on the parent supplies the timing); the real server,
whose requests come from external processes, has not reproduced it.

Full harness + gdb logs from the investigation live under the session scratchpad.
