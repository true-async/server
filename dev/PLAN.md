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
