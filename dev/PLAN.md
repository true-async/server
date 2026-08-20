# Plan

Open steps, in order. A step is closed here when its evidence is in the repository:
a test that fails without it, a measurement, or both. Issue numbers point at
`true-async/server` unless another repository is named.

## In progress

- [x] **#170 — a streamed response is flushed per chunk.** `flush` op on the encoder
  vtable (`Z_SYNC_FLUSH`, `ZSTD_e_flush`, `BROTLI_OPERATION_FLUSH`), called from
  `ws_append_chunk` for every non-empty chunk. Unconditional on the streaming path,
  no config key. Evidence: `013-h1-streaming-gzip-flush.phpt` decodes the first chunk
  while the handler is still parked and 0 bytes without the fix; `042` and `051` show
  Brotli and zstd going from nothing on the wire to a flushed block; `CompressionGzip`
  and the new `CompressionFlush` unit tests decode the flushed prefix with each
  codec's own decoder, including through the `NEED_OUTPUT` loop; cost measured in
  `dev/BENCHMARKS.md`. Not merged yet.

## Next

- [ ] **Drop the streaming exemption in laravel-spawn.** `TrueAsyncServer::streamContent`
  calls `setNoCompression()` on every `StreamedResponse` as the workaround for #170
  (YanGusik/laravel-spawn#57). Remove it once a build with the flush is in use, and
  re-measure the CSV timeline from the issue.
- [ ] **#171 — a streaming response cannot be aborted.** A body truncated by an
  exception reads as complete on the wire. Filed, deliberately not implemented.

## The response body contract (YanGusik/laravel-spawn#50, #60)

Two field reports from one user came from the API rather than from his code, and
the second arrived 44 minutes after the first was closed. `write()` buffers until
`end()`, while Node, Swoole and Go all send under that name; `sendable()` folds
four questions into one bool and `README.md:277` documents it as a liveness check,
which is the loop that truncated his stream, and which laravel-spawn had already
copied into `Sse::connected()`; a handler-declared `Content-Length` reaches the
wire unverified. The contract is settled; the steps are ordered so the
documentation lands first, because the reporter is writing a proxy recipe against
it and expects a tag within days.

- [x] **Document the three body modes first.** `docs/USAGE.md` says nothing about
  the response body at all. It gains a section naming the modes — buffered
  (`setBody`), streamed (`write`), file (`sendFile`) — the state each commits, and
  a framing table: a buffered body gets its `Content-Length` computed, an
  undeclared stream is chunked or DATA frames, a declared stream keeps the header.
  `README.md:277` and the `write()` docblock (`stubs/HttpResponse.php:160`, which
  never says that nothing leaves before `end()`) are corrected in the same step.
  Done in #174: the README guard is gone and both docblocks say what they mean.
  The `docs/USAGE.md` section landed with the renames as §3.5, written once
  against the final names.
- [x] **`isWritable(): bool` — liveness, with the op behind it.** A new optional
  `is_alive` in `http_response_stream_ops_t` (`include/php_http_server.h:706`);
  every backend already computes it inside `append_chunk` (`peer_closed` for H2,
  `stream_credit_is_dead` for the worker). Sound as a predicate because every
  input is a one-way latch, unlike queue depth.
- [x] **`write()` becomes the streaming call.** Done in #180. `send()` is removed
  outright rather than kept as a deprecated alias: it would have covered one call
  in the shipped laravel-spawn adapter while `isClosed()` breaks three others
  beside it (`src/Server/TrueAsyncServer.php:106,395,413,492`), so the adapter
  needs a release either way. Buffered incremental appending keeps its behaviour
  under `appendBody()`; `isClosed()` became `isEnded()`;
  `sendable()` throws a tombstone naming `isWritable()` and `tryWrite()`;
  `setBodyStream()`/`getBodyStream()` are deleted. Evidence:
  `tests/phpt/server/core/062-body-api-names.phpt` reads the wire for each mode,
  and `h2/023-h2-sendable-tombstone.phpt` asserts the throw on a live H2 stream
  where the method used to answer. `docs/USAGE.md` §3.5 documents the three modes.

  Two defects surfaced while doing it, both fixed here. **#181**: a buffered body
  followed by a streaming call was discarded with no error — the streaming path
  never reads `response->body`, while the reverse direction has always thrown, so
  only one side of a symmetric-looking mistake reported. **The `stream` perf
  profile had never run**: `tests/perf/servers/server_stream.php` called `->send()`
  with no argument against an arginfo requiring one, so the profile answered 500
  with `expects exactly 1 argument, 0 given` before measuring anything, and its
  chunk loop buffered through the old `write()`.
- [~] **`tryWrite(): bool` and the dialect twins.** In #178, without the twins. The non-blocking half of the
  pair, matching `WebSocket::trySend()`; `trySseEvent()` and `tryWriteMessage()`
  follow, so the idiom is not half-applied. Invariant: false means nothing was
  queued and no header was committed, and a dead peer is still the 499 exception.
  Blocked by the compressing wrapper — `ws_append_chunk` feeds the encoder and
  closes a block before it consults the underlying ops, so a refusal there is not
  retryable, and the capacity check has to move ahead of the encoder.
  Three review passes reshaped it. The refusal moved into `append_chunk` as a
  `nonblocking` argument, because a predicate read beforehand cannot be atomic at
  the PHP boundary; the wait moved into a `wait_writable` op, because each
  transport's own wait carries a deadline, a wake source and a re-pump of the
  drain that a wait assembled outside would drop; `awaitWritable()` answers false
  rather than true where a transport can be full but offers no wait, since "go
  ahead" spins a handler that trusts it. `compressing_stream_ops` and
  `h3_stream_ops` gained the missing `sendable` slots — without them a refusal
  under compression threw away a block the encoder had already emitted, and the
  retry the caller was told to make corrupted the deflate stream.

  **HTTP/1 is the open exception**: it keeps no queue of its own, so it never
  refuses and an accepted chunk waits for the socket. Two ways to close it were
  tried and rejected — see below. The twins (`trySseEvent`, `tryWriteMessage`)
  wait for that to settle.
- [ ] **Framing by declared length.** A `Content-Length` set before the first
  `write()` reaches the client verbatim on every protocol, and the server becomes
  the auditor: excess throws at the offending write, a shortfall aborts the stream
  at `end()` instead of finishing cleanly under a header that lies. Compression is
  disabled for such a response. `src/http1/http1_format.c:249` is the only path
  that passes a handler value today; H2, H3 and the worker strip it in
  `http_response_header_allowed_h2h3`. Needs the abort op from #171 — the vtable
  carries only the clean `mark_ended` (`include/php_http_server.h:723`).
- [~] **Migration.** The CHANGELOG entries are written (#180, five bullets covering
  the seven renames, plus #181 under Fixed). What is left is laravel-spawn, and it
  is five call sites rather than the two this plan assumed: `send()` → `write()`
  at `src/Server/TrueAsyncServer.php:492`, `isClosed()` → `isEnded()` at 106, 395
  and 413, and `Sse::connected()` → `isWritable()` at `src/Sse/Sse.php:42`. To land
  once a build carrying the renames is tagged. Its docblock was
  corrected ahead of the rename in YanGusik/laravel-spawn#63, so the wording stops
  teaching the loop that truncated #60 in the meantime.

## HTTP/1 has no non-blocking write, and the two candidate fixes are both wrong

`tryWrite()` cannot refuse on HTTP/1: the streaming path writes through
`http_connection_send` → `send_raw`, which submits a `uv_write` and awaits it, so
backpressure is the kernel socket buffer and there is no depth to read. Two
designs were worked out and both fail on something mechanical.

- **A second writer for the non-blocking case** (`http_connection_send_batched`,
  the one WebSocket uses). It is an unordered channel: a chunk body parked in
  `out_pending_buf` waits behind an in-flight write while the headers, a blocking
  `send()` and `mark_ended`'s terminal chunk go out through the raw path and reach
  the peer first. Chunked framing does not survive that.
- **A queue on the response** (`http1_request_ctx_t`). Dead twice over: on TLS the
  drain writer would run in scheduler context, where `tls_wait_space` refuses
  outright (`src/core/http_connection_tls.c:97`), so it could not push a byte on
  HTTPS; and the context is freed in `http_request_finalize`, while a queue drained
  by write completions outlives the handler by definition. The precedents cited for
  it — the H2 per-stream ring and the wslay FIFO — both live on connection-lifetime
  objects, not on a per-request one.

- [x] **Measure the HTTP/1 chunk path before deciding.** Taken on 2026-08-20, on a
  release PHP built for it (`dev/BENCHMARKS.md`). `strace` confirms three `write(2)`
  and three loop turns per chunk, flat in the chunk size. A one-hunk prototype that
  copies the three pieces into one awaited write gains 42.7% at four chunks, 77.3%
  at sixteen and 84.8% at sixty-four, cutting the per-chunk cost from ~18.5 µs to
  ~8.8 µs. The copy is only there because the ABI has no awaitable vectored write:
  `io_pipe_writev_cb` sends no NOTIFY and frees the request itself.

  What it decides: the win needs no queue, no second writer and no per-response
  structure, so it is not an argument for #179.

- [x] **Send a streamed chunk as one write below 32 KiB.** Merged in #184. The
  threshold is where the copy stops paying, measured: +25% at a 32 KiB chunk, −16%
  at 64 KiB, so a larger frame keeps the copy-free three-write path. Verified
  against a 9% noise floor by alternating the two builds — +81% at 4 KiB chunks,
  and no difference at 64 KiB, where both take the same path. The bound is on the
  frame rather than the chunk: `HTTP_TLS_PLAINTEXT_RING_BYTES` is also 32 KiB, so a
  chunk-sized bound made a 32 KiB chunk spend a second ring cycle on a TLS record
  carrying six bytes, and a static assert now keeps the two from drifting.

  The header block rides inside the first frame when the two fit: +28.5% on a
  one-chunk response, which is the shape of an SSE reply. `mark_ended` checks
  `stream_dead` before its header commit, so a stream that died after its headers
  landed no longer sends them twice.

  The copy stays until ext/async grows a vectored write that reports its status.
  Its release callback carries none today (`zend_async_API.h:588`) and
  `io_pipe_writev_cb` destroys the exception before calling it, so a coroutine can
  be resumed from there but cannot learn whether the write failed — and on this
  path a failed write is what makes `isWritable()` honest (#176). The same change
  would close the buffer-lifetime hazard both branches carry: libuv keeps the
  caller's pointer until its completion callback, while a cancelled request is only
  marked pending.

- [x] **Cover the three files the #177 work left behind.** The coverage gate on
  #182 flagged a drop inherited from #178 — the compressing wrapper's four
  delegating ops, the pool worker's credit-backed answers, and the HTTP/1 branches
  around the header block. `compression/052`, the trio added to `h3/047`, and two
  more shapes in `h1/029` close it: compression 75.68 → 78.38, worker_dispatch
  76.63 → 79.62, http1_stream 66.23 → 71.43.

- [ ] **Answer from the queues the connection already has.** Plaintext:
  `out_pending_buf` carries a byte count, a high-water predicate on the same knob,
  low-water hysteresis, a drain hook and a destroy defer gate — all implemented and
  all exercised by WebSocket. TLS: `BIO_ctrl_get_write_guarantee` on the plaintext
  BIO is the exact predicate `tls_wait_space` loops on, so a refusal built from it
  is exact by construction. Neither needs a new structure. What it does need is the
  out-of-band writers brought under one order first — `send_strv_owned` ignores the
  pending tail, and `emit_parse_error` writes with a direct `send(2)` syscall.
  Measure before deciding: the case for it is three submits and up to three
  suspensions per chunk, and that number has never been taken.
- [ ] **#179 — one serialized outbound path per HTTP/1 connection.** The larger
  version of the same idea. Filed, and to be judged against the measurement rather
  than against the argument.

## Landed elsewhere

- **php-async #260 / #261.** Every debug CI job broke on 2026-08-20 with
  `op_array_emalloc_copy_array: Assertion 'zend_hash_num_elements(src) ==
  src->nNumUsed'`. The helper rebuilds two tables and asserted an invariant that
  holds for one: static variables need every bucket, because BIND_LEXICAL reaches a
  captured variable by a byte offset into arData, while a literal array may hold a
  hole — `[0 => 'a', 2 => 'b']` compiles to a packed hash whose slot 1 is UNDEF.
  The assert moved to the caller whose offsets depend on it. Server commit `5392ff7`
  was green at 09:11 UTC and the same commit failed at 12:50 with nothing changed on
  its side, which is what identified the cause.

## The cmocka suite rots unnoticed

Nothing builds these targets in CI, so production signatures move and the tests keep
compiling against the old ones. Two were repaired on 2026-08-19 (`ResponseWire`: the
`bool complete` argument dropped in 6fbe731 on 2026-07-07; `TLSSession`: the ring size
the backpressure case measures is the CT-in half, `TLS_BIO_RING_SIZE_SMALL`, not the
CT-out one). `ctest` now runs 10 of 16. What is left:

- [ ] **Five targets do not link.** `test_http1_parser`, `…_edge_cases`, `…_security`,
  `test_http2_strategy`, `test_http2_session` miss `grpc_*`, `h2_*`,
  `http_connection_*`, `http_log_emit_access`, `http_response_get_*`,
  `http_server_runtime_exception_ce`, `trace_hex_encode`. The choice is per target:
  add the real TUs and accept what they drag in, or extend
  `tests/unit/common/*_stubs.c`. A stub that answers wrongly makes a green test that
  proves nothing, so the choice is not mechanical.
- [ ] **`test_http3_packet` segfaults** in `test_account_send_error_buckets`. Cause
  unknown; it is either the accounting or the test.
- [ ] **Build the unit suite in CI** once the above are green, otherwise the same rot
  returns.
