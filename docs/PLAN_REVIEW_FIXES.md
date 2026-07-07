# PLAN: review fixes — gRPC / reactor-pool reverse path / H3 body streaming

Source: 8-angle code review of `b38301e..f81e58d` (2026-07-06). Ordered by
severity; each item is an independent commit with its own test.

## P1 — Correctness

### 1. grpc-web-text: per-block base64 decode (CONFIRMED by repro)
`grpc_web_text_decode` runs one non-strict `php_base64_decode` over the whole
body. Concatenated independently-padded frames (the grpc-web protocol shape,
and exactly what our own encoder emits) decode to garbage past the first
frame whose length % 3 != 0 — PHP's decoder does not reset the bit group at
`=`. Repro: `base64(7-byte frame) . base64(8-byte frame)` → shifted bytes.
- Fix: decode block-wise — split the input at padding boundaries
  (`=`/`==` followed by more data) and decode each block, or run an
  incremental decoder that resets state after padding.
- Test: extend grpc/015 — client sends TWO frames, first with len % 3 != 0
  (e.g. message "hi"), assert both messages readable.
- File: src/grpc/grpc.c (`grpc_web_text_decode`).

### 2. UAF: `req->body_h3_conn` outlives the QUIC connection
Set in h3_end_headers_cb, never cleared. `http3_connection_free` frees the
connection while a handler coroutine still holds the request + queued body
chunks; the next `readBody()/readMessage()` → `http_body_stream_pop` →
`http3_request_body_consume(freed conn)`.
- Fix: in the connection-free force-release loop AND in normal
  `http3_stream_release` teardown, when `s->request` is still alive:
  `s->request->body_h3_conn = NULL` and, if `!fin_received`,
  `http_body_stream_error(req)`. Mind the request-lifetime rules (request
  may already be field-freed — guard on the same conditions the existing
  teardown uses).
- Note: `body_h2_session` has the same latent class on H2 — file/fix as a
  follow-up (same shape: clear on session teardown).
- Test: hard to phpt deterministically; at minimum an ASAN-able unit path or
  a phpt with idle-timeout reap while the handler sleeps mid-drain.

### 3. web-text × body_streaming: silent request loss
The issue-#26 streaming policy (H2 + H3) keys on Content-Length only; a
grpc-web-text request with CL unknown / >= 1 MiB gets `body_streaming=true`,
`req->body` stays NULL, and readMessage's web-text branch returns null
forever (queued chunks also never popped → window credit never returned).
- Fix (classify-once): exclude gRPC-web-text from the streaming policy at
  both dispatch sites — the request is buffered by protocol nature. Cleanest:
  a small predicate `http_request_body_must_buffer(req)` in the grpc layer
  (`grpc_request_is_grpc_web_text`) consulted by the H2/H3 policy blocks.
- Test: phpt with setBodyStreamingEnabled(true) + web-text request without
  Content-Length → messages still readable.

### 4. H3 streaming finalize never fires `body_event` → awaitBody() hangs
`http3_finalize_request_body` streaming branch sets complete + closes the
queue but returns before the `body_event` trigger block; a handler suspended
in `awaitBody()` before fin sleeps forever. H2 `finalize_request_body` has
the same gap; the H1 parser fires body_event explicitly in streaming mode.
- Fix: fire `req->body_event` in the streaming branch too (both H3 and H2 —
  do H2 in the same commit, it is the same three lines).
- Test: phpt — streaming-enabled H3 request >= 1 MiB, handler starts with
  awaitBody(), then getBody-less readBody drain; must not hang.

### 5. Sink STREAM_* drop: no poisoning, no telemetry, hot spin
After the bounded retry exhausts, a STREAM_CHUNK drop leaves a hole in the
body with a clean FIN; a STREAM_END drop parks the reactor's data reader on
WOULDBLOCK forever. The 1M-iteration spin has no backoff and burns a core.
- Fix:
  a. retry with `reactor_pool_msleep()`-style backoff (pattern already in
     reactor_pool_exec), far fewer iterations;
  b. on final drop, poison the stream: mark the ctx/credit dead so the
     producer stops and dispose does NOT post a clean END (see item 6 —
     share the "died mid-stream" flag);
  c. bump a counter (worker_wire_drop) so operators can see it.
- File: src/http_server_class.c (sink), src/core/worker_dispatch.c (flag).

### 6. Mid-stream death still ends the stream cleanly
When worker_stream_wait_credit fails (write timeout / cancellation), send()
throws 499 but dispose still posts a normal STREAM_END (+trailers): a live
client receives a truncated body terminated as complete.
- Fix: add `ctx->stream_failed`; set it when wait_credit fails or the sink
  reports a final drop; dispose then posts a new RESPONSE_WIRE_STREAM_ABORT
  kind (reactor: `ngtcp2_conn_shutdown_stream` / RST) instead of END.
- Test: phpt with tiny write window is hard; unit-level coverage via
  reactor_pool test hooks is acceptable.

## P2 — Architecture / duplication (SOLID)

### 7. Handler-pick + gRPC classification: one seam
The GRPC→HTTP1→HTTP2 cascade exists 5×(worker_dispatch ×2, http3_dispatch,
http2_strategy, +404 gate), and classification is expressed two ways
(worker: `grpc_request_mode(req) != NONE`; transports:
`grpc_request_is_grpc(req)`).
- Fix: `zend_fcall_t *http_protocol_pick_handler(HashTable *h, bool is_grpc)`
  in core/http_protocol_handlers.{h,c}; `grpc_mode_t grpc_classify(req,
  handlers)` in grpc.h (mode = NONE unless a gRPC handler is registered).
  Convert all five sites.

### 8. Trailer-pack duplication
`http3_stream_adopt_wire_trailers` ≅ `http3_stream_capture_trailers` (same
two-pass malloc nv+bytes packer; only the pair source differs).
- Fix: one packer taking a pair-iterator callback; both fill
  s->trailer_nv/count/bytes through it.

### 9. Credit-orphan helper
`stream_credit_mark_dead + stream_credit_release` open-coded at 3 sites.
- Fix: `static inline void stream_credit_abandon(stream_credit_t *sc)`
  (NULL-safe) in stream_credit.h; consider `response_wire_discard(rw)` that
  abandons an attached credit so future drop-sites are safe by construction.

### 10. Shared coroutine-sleep helper
`worker_credit_sleep_ms` duplicates `hot_reload_sleep_ms` (already diverging
on EG(exception) handling); both are also the only park sites without
ZEND_ASYNC_WAKER_NEW (works via enqueue auto-create, but non-idiomatic).
- Fix: one helper in src/core/ (e.g. `http_async_sleep_ms(co, ms)`), used by
  both; align the waker idiom with the rest of the codebase.

### 11. Header-flatten loop
The "foreach allowed h2h3 header, string-or-array" flatten exists ~6×
(worker_wire_copy_head, http2_strategy ×2, http2_static_response ×2,
http3_callbacks).
- Fix: `http_response_foreach_allowed_header(resp, emit_cb, arg)` next to
  the allowed_h2h3 predicate; convert at least the new worker site now,
  transports opportunistically.

## P3 — Efficiency (reverse path)

Context: the first iteration deliberately optimized for correctness and
thread-domain safety (arena wires are the established malloc-clean pattern;
the 2ms poll mirrors hot_reload_sleep_ms; per-pop drain mirrors the local
path's per-chunk drive). These are the follow-ups now that the semantics
are pinned by tests.

### 12. Chunk payload: one copy instead of three
zend_string → wire arena (worker) → zend_string_init (reactor). The arena
copy is inherent to response_wire, but the payload doesn't have to ride the
arena: carry a persistent (malloc) zend_string ref on the wire
(`response_wire_set_chunk(rw, zend_string *persistent)`), adopt it directly
into the chunk ring on the reactor (ring already owns refs; release stays
same-thread because persistent strings are thread-agnostic). Result: exactly
one copy (handler's ZMM string → persistent buffer).

### 13. Credit poll: reusable timer + adaptive interval
Per-poll ZEND_ASYNC_NEW_TIMER_EVENT create/start/dispose ≈ 500 alloc cycles
per parked second. Reuse one timer event per dispatch ctx (rearm), and/or
back off the interval (2 → 4 → 8 ms capped) while the credit level is
unchanged.

### 14. Body-consume credit coalescing
`http3_request_body_consume` runs drain_out + arm_timer per popped ~1.2 KB
chunk. Extend the window per pop (cheap), but flush (drain_out) only when
retired bytes since the last flush >= half the stream window — the same 50%
rule the H2 consume path uses; alternatively defer to the reactor's
drain-epilogue.

### 15. STREAM_CHUNK apply: batch drain
Per-apply resume+drain_out+arm_timer defeats GSO batching when several
chunks land in one mailbox drain. Mark the connection dirty and drain once
per batch via the existing reactor drain epilogue
(`reactor_pool_set_drain_epilogue` — the H3 steer flush already coalesces
this way).

## P4 — Docs / comments (one commit)

### 16. Stale contracts sweep
- include/core/worker_dispatch.h: "Buffered responses only for now" → the
  streaming reverse path + credit description.
- src/core/worker_dispatch.c ops-block comment + src/http_server_class.c
  sink comment: drop "step-3 follow-up / interim until credit" wording;
  state what the retry is still load-bearing for (HEADERS/END, many small
  streams).
- include/core/response_wire.h: `complete` flag has no readers — either
  document it as vestigial-for-FULL or remove `response_wire_body_complete`.
- include/http_body_stream.h: pop now performs transport I/O (deferred
  QUIC/H2 credit) and readMessage is a second consumer; the "same thread"
  contract must say "the connection's thread".
- include/http2/http2_stream.h + include/http3/http3_stream.h: `grpc_web`
  fields are write-only after the mode change — mark or remove.
