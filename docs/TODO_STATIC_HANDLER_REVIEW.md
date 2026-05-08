# Static handler — review findings & follow-up tasks

Review date: 2026-05-07. Branch: `13-built-in-static-file-handler`.
Reference document: `docs/PLAN_STATIC_HANDLER.md` (read end-to-end first).

This file is the actionable distillation of a code-quality / performance
audit done over the static-handler PR. Pick items in priority order; the
🔴 ones are merge-blockers, the rest can land incrementally.

---

## Background you need before touching anything

- Implementation lives in `src/static/` and the dispatch hook in
  `src/core/http_connection.c::http_connection_dispatch_request` around
  line 1622.
- Two serve paths exist:
  - **Hard-zero async chain** (`ss_kick_off` → `ss_dispatch` FSM in
    `src/static/http_static.c:641-721`): plain TCP only, callback chain
    rooted at `ZEND_ASYNC_FS_OPEN`, no coroutine, ends in
    `http_request_finalize`.
  - **Sync slurp fallback** (same file, `try_open_candidate` +
    `slurp_fd` near `:830-988`): synchronous `open` + `fstat` +
    blocking `read()` up to 256 MiB. Used for TLS, directory+index walk,
    and `on_missing: Next` mounts.
- Per-request lifecycle counters: `http_server_on_request_dispatch` is
  paired with `http_server_on_request_dispose`;
  `http_server_count_request` is the "we sent a response" bump.
- Worker pool transfer is in `http_server_transfer_obj`
  (`src/http_server_class.c:2644`). It explicitly walks `protocol_handlers`
  via a `transit_handlers` sidecar but **does not currently transfer the
  static_handler arrays**.
- PHPTs to extend / use as smoke tests:
  `tests/phpt/server/static/{001,002,003}-*.phpt`.

---

## 🔴 Merge-blockers

### ✅ Done items

- **#1 Worker-pool transit drops static_handlers** — fixed by switching
  the server's mount storage from "pointer-into-PHP-object" to a
  refcounted persistent shared snapshot. `addStaticHandler()` now calls
  `http_static_handler_freeze()` which deep-copies into pemalloc with
  persistent zend_strings + persistent HashTables, refcount=1. The
  worker-pool TRANSFER becomes a side-car of pre-addref'd shared
  pointers; LOAD addrefs once more into each worker's emalloc array.
  `http_server_free` releases shared refs via
  `http_static_handler_shared_release` (atomic decref → destroy on 0).
  Public API: `http_static_handler_freeze`,
  `http_static_handler_shared_addref`,
  `http_static_handler_shared_release` in `static_handler.h`. Test:
  `tests/phpt/server/static/004-static-workers.phpt` (workers=2).

- **#2 Counter mismatch on `ZEND_ASYNC_IO_SENDFILE` submit failure** —
  `http_server_count_request` is now bumped before `ss_finalize` on the
  submit-failure branch (`src/static/http_static.c:628`).

### ~~1. Worker-pool transfer drops static_handlers~~ ✅ done

`http_server_transfer_obj` in `src/http_server_class.c:2644-2713` copies
`config` and `protocol_handlers` into a persistent transit, then a LOAD
in the worker thread re-hydrates them. **Neither the TRANSFER nor the
LOAD path touches `static_handler_mounts` / `static_handler_objects`.**

Effect: in any deployment using `workers > 1`, every worker clone has
`static_handler_count == 0`, so `http_static_try_serve` always returns
PASSTHROUGH. Static requests fall through to the PHP handler, or to the
synthetic 404 at `http_connection.c:1643-1651` if no PHP handler exists.

Fix sketch:

- Add `transit_static_mounts` (analogous to `transit_handlers`) holding
  pemalloc'd snapshots of each mount descriptor: prefix, root,
  cache_control, hide_globs, index_files, mime_overrides, extra_headers,
  flags. All zend_string fields must be persistent-copied
  (`zend_string_init_persistent` / `zend_string_dup` with persistent=1).
- LOAD path constructs a fresh `TrueAsync\StaticHandler` PHP object per
  worker using the destination thread's heap, fills its embedded
  descriptor from the transit snapshot, locks it, registers it on the
  worker's server via the existing `addStaticHandler` storage-array
  growth code (refactor that block out of the userland method into a
  C-only helper).
- Mirror the `protocol_mask |= HTTP_PROTO_MASK_HTTP1` set that the
  userland method does.

Acceptance:

- New PHPT: spawn `workers=2`, hit `/static/foo.css` from each, verify
  both return 200 with the file body. (Workers serialize the start so
  the parent test can hit each one.)
- Existing 001/002/003 still pass.

### ~~2. Counter mismatch on `ZEND_ASYNC_IO_SENDFILE` submit failure~~ ✅ done

`src/static/http_static.c:625-630`:

```c
state->pending_req = ZEND_ASYNC_IO_SENDFILE(
        state->conn->io, state->file_io, 0, (size_t) state->st.st_size);
if (UNEXPECTED(state->pending_req == NULL)) {
    ss_finalize(state);          /* count_request never called */
}
```

Every other terminal in the FSM bumps `http_server_count_request` before
`ss_finalize`. This branch doesn't — `total_requests` undercounts on
sendfile-submit failure.

Fix: add `http_server_count_request(state->conn->counters);` immediately
before the `ss_finalize(state)` on this branch. Or refactor: move the
count_request bump into `ss_finalize` itself and remove the per-terminal
calls (then audit every pre-existing terminal for double-counting).

Acceptance: code review only; not easy to PHPT a sendfile-submit failure.

---

## 🟠 Should-fix-soon

### 3. Headers + sendfile ordering race on slow socket

Plan §7 open question 6 already names this. The handler currently does
fire-and-forget `ZEND_ASYNC_IO_WRITE_EX` for headers (going through the
batched/pending uv_write queue) and immediately submits
`ZEND_ASYNC_IO_SENDFILE` against the same `conn->io`. If the headers
write is queued (out_in_flight) and sendfile writes via a separate code
path on the same fd, byte ordering on the wire is undefined.

Pick one of:

- **TCP_CORK** at the entry of `ss_kick_off` (or first send), uncorked
  in `ss_finalize`. One extra syscall per response, zero correctness
  risk.
- **Wait for the headers `uv_write` callback** before submitting
  `uv_fs_sendfile`. Adds one round through the loop but no syscall.
- **Serialise via `conn->out_in_flight`**: submit sendfile only when
  `out_in_flight == false`, otherwise stash and let
  `http_send_batched_completion_cb` kick it. Most invasive but reuses
  the existing batching machinery.

Acceptance: under `tc qdisc add dev lo root netem delay 50ms` plus
small TCP send buffer, hammer with `wrk -c 1 -d 30 /static/big.bin` and
verify response bytes are byte-identical to the file in 10/10 runs.

### 4. ~~Suspected callback leak~~ → false alarm; valgrind verification only

**Original suspicion was that `del_callback` only unhooks the callback
without disposing it, so `event.dispose` later wouldn't free our `cb`.
Resolved by reading `~/php-src/Zend/zend_async_API.c:1625-1642`:
`zend_async_callbacks_remove` (which `libuv_remove_callback` aliases via
`event.del_callback`) explicitly calls `callback->dispose(callback,
event)` on line 1637 before returning.** So `ss_cb_dispose` runs
correctly. No leak in this path.

Remaining task: run `valgrind --leak-check=full` on the static PHPTs
to confirm no other path leaks (independent of this item — already in
plan §6 acceptance).

### 5. Sync slurp_fd is the default for non-trivial parts of the matrix

Hard-zero is gated on (not-directory ∧ not-TLS ∧ not-on_missing-Next ∧
realpath-under-root) at `http_static.c:818-825`. Everything else falls
through to the synchronous slurp at `:880-941`, which does blocking
`read()` on the loop thread up to `HTTP_STATIC_MAX_FILE_SIZE` (256 MiB).
TLS is the hottest part of that fallthrough — it's the default in real
deployments — so the bulk of the work goes here.

The fix splits into three independent sub-tasks. Each can land in its
own commit. **Order matters: 5a (TLS) gives the biggest user-visible
win; 5b (index-walk) is a cheap easy follow-up; 5c (on_missing:Next)
unlocks the last remaining path.**

#### 5a. TLS hard-zero via userspace SSL_write FSM

No new `ext/async` API needed — the helper lives in `tls_layer.c`.
Three branches replace the current `conn_supports_sendfile`:

```
if (kTLS TX engaged on this conn)
    → existing sendfile path (kernel encrypts in-place)
else if (TLS user-space)
    → new READ → SSL_write → drain → uv_write FSM (this task)
else (plain TCP)
    → existing sendfile path
```

The user-space FSM cycles per chunk (default 64 KiB):

```
ZEND_ASYNC_IO_READ(file_io, buf, off, n) → on_read →
    SSL_write(buf, n)                            (CPU-only, no syscall)
    drain ciphertext_bio_app via BIO_read
    fire-and-forget submit ciphertext to conn->io
    on ciphertext-write completion → next ZEND_ASYNC_IO_READ
```

Plumbing already in the codebase to leverage:

- `conn->tls_plaintext_bio` / `tls_plaintext_bio_app` — the BIO_pair.
  See `http_server_class.c:2216-2225`.
- `tls_layer.c` already does `SSL_write` → drain → write. We need a
  fire-and-forget version (the current path likely await-suspends on
  ring fullness — verify).
- `http_connection_send_str_owned` is the fire-and-forget plaintext
  send model; mirror that for ciphertext.

State machine extends `ss_phase_t` with two new phases:

```
SS_PHASE_TLS_READ        /* awaiting ZEND_ASYNC_IO_READ chunk */
SS_PHASE_TLS_WRITE_DRAIN /* awaiting ciphertext socket write completion
                            before reading the next chunk — gates SSL_write
                            to avoid BIO_pair fullness (SSL_ERROR_WANT_WRITE) */
```

Backpressure gotcha: the BIO_pair is fixed at
`HTTP_TLS_PLAINTEXT_RING_BYTES` (16 KiB plaintext). If we `SSL_write` a
64 KiB chunk and the ciphertext hasn't drained, `SSL_write` returns
`SSL_ERROR_WANT_WRITE`. So the FSM must size chunks to fit, or split
within a single chunk. Simplest: 16 KiB read chunks (matches typical
TLS record size and the BIO ring), one SSL_write per chunk, one
ciphertext drain + uv_write per chunk, gated wait for completion.

Acceptance:

- New PHPT under TLS that serves a 4 MiB file and verifies bytes match.
- Concurrent ping request lands within ~1 RTT during the transfer
  (loop not blocked).
- `tls_ktls_tx_total` counter still bumps on kTLS-engaged sessions
  (regression guard for the fast path).

Open API question to verify before starting:
[See "API questions to verify" section at the end of this file.]

#### 5b. Index-walk via sync stat-only offload + async open

Replace the current sync `try_open_candidate` loop with a sync
**stat-only** helper, then a single async `ZEND_ASYNC_FS_OPEN` of the
found name:

```c
/* Returns the matching index_files[i] zend_string * (borrowed) or NULL. */
const zend_string *http_static_find_index(const http_static_handler_t *mount,
                                          char *dir_path, size_t dir_path_len,
                                          size_t dir_path_cap);
/* Mutates dir_path in-place: appends each candidate name, stat()s it,
 * truncates back. On hit, leaves dir_path with the joined path so the
 * caller can immediately FS_OPEN it. */
```

Why stat-only is a big win even staying sync: cold-cache stat is an
inode lookup (microseconds); cold-cache slurp+read of a multi-MiB
index.html is milliseconds. The expensive work (read + send to socket)
moves to async sendfile. Stat-only sync stays.

Future micro-optimisation: if `ZEND_ASYNC_FS_STAT(path, ...)` exists in
the API surface, swap the sync stat for it — fully async path with no
loop blocking. Until then, sync stat is acceptable.

Acceptance:

- Existing 001-static-basic.phpt (which exercises index resolution)
  passes.
- New PHPT: directory request + index file → 200 served via hard-zero
  (verify by static_zero_coroutine_total counter — see plan §6).

Open API question:
[See "API questions to verify".]

#### 5c. on_missing:Next graceful-rollback from hard-zero

The current `ss_kick_off` gate excludes `on_missing:Next` mounts
because once we commit to hard-zero, falling back on ENOENT looks
impossible. It is possible — just intricate. In `ss_handle_open` on
ENOENT for an `on_missing:Next` mount:

```c
/* 1. Reverse ss_kick_off side-effects */
http_server_on_request_dispose(state->conn->counters);
state->conn->handler_refcount--;

/* 2. Tear down file_io (the open already returned an error) */
if (state->cb) state->file_io->event.del_callback(...);
state->file_io->event.dispose(...);

/* 3. Spawn a coroutine and reuse the existing ctx (request_zv /
 *    response_zv have not been touched — open ENOENT'd before any
 *    write to ctx). */
zend_coroutine_t *coro = ZEND_ASYNC_NEW_COROUTINE(state->conn->scope);
coro->internal_entry    = http_handler_coroutine_entry;
coro->extended_data     = state->ctx;
coro->extended_dispose  = http_handler_coroutine_dispose;
state->ctx->request->coroutine = coro;

/* 4. Re-do what http_connection_dispatch_request would have done */
http_server_on_request_dispatch(state->conn->counters);
state->conn->handler_refcount++;
ZEND_ASYNC_ENQUEUE_COROUTINE(coro);

/* 5. Drop state — ctx ownership transferred to the coroutine */
efree(state);
```

Once this works, the `!(mount->flags & HTTP_STATIC_FLAG_ON_MISSING_NEXT)`
gate in the `ss_kick_off` condition can be removed. Mounts with
`on_missing:Next` then get hard-zero on the success path and graceful
rollback on miss — best of both.

Acceptance:

- 002-static-dotfile-and-onmissing.phpt still passes (and exercises
  the rollback via a missing file under `on_missing:Next`).
- New PHPT: existing file under `on_missing:Next` mount → served via
  hard-zero (verify via counter).

Open API questions:
[See "API questions to verify".]

### 6. Per-request HashTable foreach for `extra_headers` / `cache_control`

`apply_extra_headers` (sync path) and `ss_build_headers` (hard-zero
path) both do `ZEND_HASH_FOREACH_STR_KEY_VAL(mount->extra_headers, ...)`
with a per-iteration `strncasecmp("content-", ...)` for 304-eligibility.

After `http_static_handler_lock`, the mount is read-only. Pre-render at
lock-time:

- `mount->prebaked_headers_full` (zend_string, persistent): all
  extra_headers + Cache-Control joined into "Name: value\r\n" lines.
  Used on 200/206/etc.
- `mount->prebaked_headers_no_content` (zend_string, persistent): same
  set minus `Content-*`. Used on 304.

Hot path becomes one `smart_str_append(buf, prebaked)` instead of
foreach + N append_header calls. Saves N hash slot reads + N strncasecmps
per response.

Acceptance: existing PHPTs still pass (header bytes byte-identical);
`wrk` measurement shows reduced per-request CPU.

---

## 🟡 Nice-to-have / cleanup

### 7. `phase` / `pending_req` race window

`http_static.c:535-541` (and the equivalent SENDFILE block):

```c
state->phase       = SS_PHASE_STAT;
state->pending_req = ZEND_ASYNC_IO_STAT(state->file_io, &state->st);
```

If the submit completed synchronously and fired the persistent callback
inline between those two lines, the callback would observe
`(phase=STAT, pending_req=NULL)` and silently `return` — state machine
deadlocks because nothing else will fire. Today libuv's
`uv_fs_stat`/`uv_fs_sendfile` always thread-pool, so this is theoretical
— but it's a footgun.

Fix: drop the req-identity guard entirely (phase alone is sufficient for
a linear FSM), or use a sentinel like `(zend_async_io_req_t *)0x1`
between the two assignments.

### 8. `prefixed[PATH_MAX]` scratch buffer is unnecessary

`src/static/http_static_path.c:163-176`: builds `prefixed` solely to
prepend a `/` before calling `validate_segments`. But `validate_segments`
already tolerates either form (`size_t i = (path_len > 0 && path[0] == '/') ? 1 : 0;`).
Replace with `validate_segments(mount, decoded, decoded_len)`. Saves
4 KiB stack + memcpy per request.

### 9. `is_hidden` scratch-buffer copy is unnecessary

`src/static/http_static_path.c:223-245`: copies `relative` into a
`scratch[PATH_MAX]` buffer to NUL-terminate. But `out_buf` already has a
NUL right after `relative`'s end (see `:193`). Pass `relative` straight
to `fnmatch`. Saves a per-request memcpy proportional to path length.

### 10. Status-line table duplication

`ss_status_line` (http_static.c:341-352) hard-codes 6 status strings.
`http11_status_lines[]` in `src/http_response.c:1207-1229` has the same
codes plus more. Expose an accessor (`http_response_status_line(int code,
size_t *len)`) and drop `ss_status_line`.

### 11. Inline `http_static_handler_count` / `_get`

Currently regular function calls in `src/http_server_class.c:770-782`.
Hot-path cost is one indirect call + one load. Either:

- Move to `static inline` in a public header by exposing the
  `http_server_object` field offsets (clean: a tiny accessor struct
  pointer returned via one function call cached at conn create).
- Or accept the call cost and document it (the bench will show whether
  it matters).

### 12. MIME comment vs implementation

`include/static/static_handler.h:78` says built-in is consulted first;
`src/static/http_static_mime.c:171-188` consults overrides first. The
override-wins semantics is the right user-visible behaviour. Update the
comment in the header.

### 13. `ss_state_t.fs_path[PATH_MAX]` is fat

4 KiB inline per in-flight static request. With 10 K concurrent, that's
40 MiB just for path strings, most of which are <100 bytes. Switch to
`emalloc(fs_path_len + 1)` and store a `char *fs_path` + `size_t
fs_path_len`. Free in `ss_finalize` before `efree(state)`.

### 14. Directory request → 301 redirect, not 404

URL `/static/foo` where `foo` is a directory currently 404s
(`S_ISREG` failure inside `try_open_candidate`). Most static servers
redirect to `/static/foo/` (301) so relative URLs in the served HTML
resolve correctly. Either implement, or document the deviation in the
StaticHandler stub.

### 15. `root="/"` silent failure

`resolved_under_root` requires `canonical[root_len] == '\0' || '/'`.
With `root="/"` (root_len=1), `canonical="/etc/passwd"` has
`canonical[1]='e'` → always false → mount serves nothing. Reject in
`canonicalise_root_directory` with a clear error message.

---

## Tests / validation gaps from plan §6

These are listed in the plan's Acceptance checklist as outstanding;
fold them into this task file so they don't get lost:

- `If-Modified-Since` past mtime → 304 (parser is implemented; PHPT
  only covers `If-None-Match`). Add a phpt under `tests/phpt/server/static/`.
- Telemetry counter `static_zero_coroutine_total` — needs the counter
  itself plus a PHPT that hits a static URL and asserts the counter
  bumped.
- `wrk -c 256 -t 4 -d 30 /static/main.css` baseline vs `entry.php` map.
  Numbers need to land in the plan or in a new BENCH.md so the "5-15%
  gain" claim has evidence.
- Valgrind on a million-request run — existing CI target exists, just
  needs to be pointed at the static suite and added to the pipeline.

---

## Out of scope for this task list

- PR #2 (H2/H3 integration via nghttp2/nghttp3 data provider) — separate
  PR per plan.
- PR #3 (Range requests).
- PR #4 (precompressed sidecars).
- PR #6 (browse).
- `SYMLINKS_OWNER` real implementation (plan §7 open question 7).

(TLS hard-zero is now in scope — folded into task #5a above. Plan §7
open question 5 is being closed by this work.)

---

## API answers (resolved by grep over ~/php-src @ branch true-async + ~/php-src/ext/async @ main, 2026-05-07)

For 5a (TLS hard-zero):

- **Q1 — non-suspending TLS write helper.** ✅ Already exists. Three
  building blocks in `src/core/tls_layer.c`:
  - `tls_write_plaintext(session, buf, len, *written)` — calls
    `SSL_write_ex`. CPU-only, never suspends. Returns `TLS_IO_OK` /
    `TLS_IO_WANT_WRITE` / etc. on backpressure (decl `tls_layer.h:213`).
  - `tls_peek_cipher_out(session, out_ptr) → bytes` — zero-copy
    pointer into the ciphertext side of the BIO_pair (decl `:255`).
  - `tls_drain_ciphertext(session, buf, cap, *produced)` — alternative
    that copies (decl `:210`).

  No new ext/async API needed. The FSM uses `tls_write_plaintext` →
  `tls_peek_cipher_out` → fire-and-forget `ZEND_ASYNC_IO_WRITE_EX(conn->io,
  ciphertext, n, free_cb)`. Mirrors `http_connection_send_str_owned`.
- **Q2 — kTLS TX detection.** ✅ `tls_session_ktls_tx_active(session)`
  in `tls_layer.c:950-956`. Probes `BIO_get_ktls_send` runtime — true
  iff kernel offload is engaged on this socket. Drop into
  `conn_supports_sendfile`:

  ```c
  static inline bool conn_supports_sendfile(const http_connection_t *c) {
      if (c->tls == NULL) return true;             /* plain TCP */
      return tls_session_ktls_tx_active(c->tls);   /* kernel encrypts */
  }
  ```

  All three branches (plain TCP, kTLS, user-space TLS) collapse cleanly:
  if `conn_supports_sendfile` → existing hard-zero. Otherwise →
  user-space TLS FSM.

For 5b (Index-walk):

- **Q3 — FS_STAT by path.** ❌ Not exposed. The async filesystem
  surface in `Zend/zend_async_API.h` is:
  - `ZEND_ASYNC_FS_OPEN(path, flags, mode)` — path-based.
  - `ZEND_ASYNC_IO_STAT(io, buf)` — io-based (requires open fd already).
  No `ZEND_ASYNC_FS_STAT(path)` exists. So sync stat is the right
  fallback and stays. Adding `fs_stat_t` to the public API later is
  trivial (libuv already has `uv_fs_stat`) — file as separate ticket
  against `~/php-src/ext/async` if the bench shows index-walk hot.

For 5c (on_missing:Next rollback):

- **Q4 + Q5 — NEW_COROUTINE / ENQUEUE_COROUTINE from a callback.** ✅
  Both are unconditional function-pointer dispatched calls
  (`zend_async_API.h:2540`, `:2545`) with no context preconditions.
  Definitive proof of legality: `src/http3/http3_dispatch.c:134` already
  does `ZEND_ASYNC_NEW_COROUTINE(scope)` from inside the per-stream
  dispatch callback — same callback class as ours. The on_missing:Next
  rollback design from §5c is sound.

For 4 (cb leak):

- **Q6 — `del_callback` dispose semantics.** ✅ NOT leaking. In
  `~/php-src/Zend/zend_async_API.c:1625-1642`, `zend_async_callbacks_remove`
  (which is what `libuv_remove_callback` aliases via `event.del_callback`)
  explicitly does `callback->dispose(callback, event);` on line 1637
  before returning. So `ss_cb_dispose` runs and `efree(cb)` happens.
  **Item #4 in this file is a false alarm — the code is correct as
  written. Demote to a verification task: re-check under valgrind to
  confirm no other leak path.**

---

## Implementation-level open questions (think about tomorrow)

These are not "should we do it" questions — those are settled. These
are "how exactly do we lay out the details" questions that surface
when writing code. Resolve at design time of each respective task.

For #5a (TLS user-space FSM):

- **IQ1. BIO_pair size.** The plan calls for 16 KiB read chunks
  "matching the BIO ring". Verify the actual value of
  `HTTP_TLS_PLAINTEXT_RING_BYTES` (declared somewhere in `tls_layer.h`
  or `http_connection.h`). If it's 64 KiB, chunks can be larger →
  fewer iterations, less overhead. If it's 16 KiB, the plan stands.
  Find: `grep -rn HTTP_TLS_PLAINTEXT_RING_BYTES src/ include/`.
- **IQ2. peek vs drain for ciphertext.** `tls_peek_cipher_out` returns
  a zero-copy pointer into the BIO; `tls_drain_ciphertext` copies. If
  we use peek and feed the pointer to `ZEND_ASYNC_IO_WRITE_EX`, the
  BIO bytes must stay live until uv_write completes. Is there a
  `tls_commit_cipher_out_consumed(n)` (or similar) that signals "uv_write
  done, BIO can advance the read head"? If not, the choices are
  (a) peek + memcpy into our own emalloc'd buffer (one extra memcpy,
  but simple), or (b) extend `tls_layer.c` with a peek/commit pair.
  (a) is fine for v1.

For #5c (on_missing:Next rollback):

- **IQ3. ctx reuse race surface.** When we roll back in the FS-callback
  (`ss_handle_open` ENOENT), `ctx->request_zv` and `ctx->response_zv`
  are still untouched (open ENOENT'd before any write). But
  `request->coroutine` was NULL when ctx was created in the
  read-callback, and gets set in our rollback path before
  ENQUEUE_COROUTINE. Verify that nothing checks `request->coroutine`
  between ctx creation and the rollback — e.g. cancel paths, parse-error
  paths, deadline-tick. Audit by `grep -n "request->coroutine\|req->coroutine"
  src/`.

For #1 (worker-pool transit):

- **IQ4. Persistent zend_string handling for `extra_headers` /
  `mime_overrides`.** Standard pattern is per-key/per-value
  `ZEND_ASYNC_THREAD_TRANSFER_ZVAL` (one transfer call each, mirroring
  how protocol_handlers transit closures). Cheap, but pemalloc churn
  scales with header count. Alternative: serialize whole HashTable
  into a single persistent buffer and re-parse in LOAD. Probably not
  worth it — header counts are tiny.
- **IQ5. Need PHP StaticHandler object on workers, or just C mount?**
  The PHP-facing `TrueAsync\StaticHandler` object is only meaningful
  to user code at config time (before `start()`). On a worker thread
  there's no user code touching it. Likely we can skip materialising
  PHP objects on workers entirely — just allocate `http_static_handler_t`
  in worker-thread `emalloc`, fill from the persistent transit
  snapshot, and stash on `static_handler_mounts[]`. `static_handler_objects[]`
  on worker stays NULL. This avoids re-running validators / locking /
  PHP class machinery. Verify nothing on the dispatch hot path touches
  `static_handler_objects[i]`.

---

## Suggested order of work

API prep is **already done** — answers in the "API answers" section
above. No grep pass needed before starting; just open this file.

1. (Day, 🔴) Fix the worker-pool transit gap (#1) + add multi-worker PHPT.
2. (Minutes, 🔴) Counter fix on sendfile-submit failure (#2).
3. (Day, 🟠) Decide and implement headers+sendfile ordering (#3). The
   TLS path through #5a doesn't have this race (everything goes through
   one ciphertext queue), so this only affects plain TCP. TCP_CORK is
   the lowest-risk path; pick that unless bench shows otherwise.
4. (Half a day, 🟠) Pre-render headers at lock-time (#6).
5. (1-2 days, 🟠) **#5a — TLS hard-zero FSM** (kTLS bypass via
   `tls_session_ktls_tx_active` + user-space SSL_write FSM using
   `tls_write_plaintext` / `tls_peek_cipher_out`). Biggest perf win.
6. (Half a day, 🟠) **#5b — index-walk sync-stat-only offload + async
   open of the found candidate.** Sync stat stays (no FS_STAT-by-path
   in current API).
7. (Half a day, 🟠) **#5c — on_missing:Next rollback from hard-zero.**
   Removes the last gate from `ss_kick_off`. Mounts with
   `on_missing:Next` get hard-zero on success, graceful coroutine
   fallback on miss.
8. (Hours) Run static PHPTs under valgrind to close out #4 verification.
9. Sweep through 🟡 cleanups (#7-#15) in a single small PR.
10. Close the validation gaps (PHPTs, valgrind, bench numbers).
