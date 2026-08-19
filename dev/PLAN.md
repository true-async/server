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

- [ ] **Document the three body modes first.** `docs/USAGE.md` says nothing about
  the response body at all. It gains a section naming the modes — buffered
  (`setBody`), streamed (`write`), file (`sendFile`) — the state each commits, and
  a framing table: a buffered body gets its `Content-Length` computed, an
  undeclared stream is chunked or DATA frames, a declared stream keeps the header.
  `README.md:277` and the `write()` docblock (`stubs/HttpResponse.php:160`, which
  never says that nothing leaves before `end()`) are corrected in the same step.
- [ ] **`isWritable(): bool` — liveness, with the op behind it.** A new optional
  `is_alive` in `http_response_stream_ops_t` (`include/php_http_server.h:706`);
  every backend already computes it inside `append_chunk` (`peer_closed` for H2,
  `stream_credit_is_dead` for the worker). Sound as a predicate because every
  input is a one-way latch, unlike queue depth.
- [ ] **`write()` becomes the streaming call.** `send()` stays one minor release as
  a deprecated alias; buffered incremental appending keeps its behaviour under
  `appendBody()`; `isClosed()` becomes `isEnded()`, which is all it ever reported;
  `sendable()` is removed with a tombstone naming its two replacements, because
  shipped adapter code calls it; `setBodyStream()`/`getBodyStream()`
  (`stubs/HttpResponse.php:257,265`) are deleted — one throws "not yet
  implemented", the other returns null.
- [ ] **`tryWrite(): bool` and the dialect twins.** The non-blocking half of the
  pair, matching `WebSocket::trySend()`; `trySseEvent()` and `tryWriteMessage()`
  follow, so the idiom is not half-applied. Invariant: false means nothing was
  queued and no header was committed, and a dead peer is still the 499 exception.
  Blocked by the compressing wrapper — `ws_append_chunk` feeds the encoder and
  closes a block before it consults the underlying ops, so a refusal there is not
  retryable, and the capacity check has to move ahead of the encoder.
  `compressing_stream_ops` (`src/compression/http_compression_response.c:701`) has
  no `sendable` slot either, so under compression the answer is a constant true.
- [ ] **Framing by declared length.** A `Content-Length` set before the first
  `write()` reaches the client verbatim on every protocol, and the server becomes
  the auditor: excess throws at the offending write, a shortfall aborts the stream
  at `end()` instead of finishing cleanly under a header that lies. Compression is
  disabled for such a response. `src/http1/http1_format.c:249` is the only path
  that passes a handler value today; H2, H3 and the worker strip it in
  `http_response_header_allowed_h2h3`. Needs the abort op from #171 — the vtable
  carries only the clean `mark_ended` (`include/php_http_server.h:723`).
- [ ] **Migration.** Seven BC entries in the CHANGELOG. laravel-spawn is a two-line
  diff: `Sse::connected()` calls `isWritable()`, `send()` becomes `write()`. Tell
  YanGusik separately that the `connected()` docstring is wrong today, before the
  rename rather than after.

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
