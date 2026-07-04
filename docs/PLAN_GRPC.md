# gRPC support — implementation plan (issue #4)

_Status: living document. Last updated 2026-07-04._

## Progress

- **Phase 0 — DONE.** Streaming-response trailers now emit correctly
  (`h2_dp_mark_eof` submits response trailers at true EOF, inside nghttp2's
  send loop, once all DATA has drained). Also fixed: `setTrailer/setTrailers/
  resetTrailers` were wrongly blocked after `send()` — they are now allowed
  on a committed streaming response (only `end()`/`sendFile()` seal them),
  which the code comment already intended and gRPC server-streaming requires.
  Test: `tests/phpt/server/h2/027-h2-streaming-trailers.phpt`.
- **Phase 1 — DONE (unary).** `application/grpc` routing → `addGrpcHandler`;
  a gRPC-only server (no `addHttpHandler`) is accepted; `src/grpc/` framing
  codec; `HttpRequest::readMessage()` / `HttpResponse::writeMessage()`;
  auto `content-type: application/grpc`; default `grpc-status: 0`;
  Trailers-Only for bodiless errors. Tests: `tests/phpt/server/grpc/001` (unary
  echo) and `/002` (error status, Trailers-Only). Full h2 + core suites green.
- **Phase 2 — DONE (all four RPC shapes), no new C code.** Server-streaming,
  client-streaming, and bidi all fall out of the Phase 0 + Phase 1 machinery
  (`writeMessage` → streaming send; `readMessage` loop over the deframer;
  EOF trailers). Tests: `/004` (server-streaming, N framed responses), `/005`
  (client-streaming, N request messages), `/006` (bidi half-duplex, N-in/N-out
  on one stream).
  **Deferred to a Phase 2b follow-up** (coupled + substantial, not yet needed by
  the buffered path): true *full-duplex interleaved* bidi — reading each message
  as it arrives while writing — needs `readMessage()` wired to the incremental
  `http_body_stream` (issue #26) instead of the buffered `req->body`, and with it
  the body-cap fix (§4.3, the cumulative lifetime cap in `http_body_stream.c`).
  gRPC uses the buffered request body today, so §4.3 does not bite yet;
  server/client/half-duplex-bidi work fully.
- **Phase 3 — DONE (propagation); hard auto-cancel deferred.** `grpc-timeout`
  is parsed (`grpc_parse_timeout_ns`) and exposed to handlers via
  `HttpRequest::getGrpcTimeout(): ?float` (seconds), so a handler can bound its
  own work against the client deadline. Test: `/007`. **Deferred:** server-side
  auto-cancel of the handler when the deadline elapses — that injects a
  cancellation into the coroutine (the same unwind path that leaks a handle in
  #101), so it is coupled to that fix; and server-emitted `DEADLINE_EXCEEDED`
  is not required for interop (the client enforces its own deadline).
- **Phase 4 — DONE (per-message gzip), reusing the existing compression
  backend.** No second zlib wrapper: the one-shot buffer helpers live in
  `src/compression/` and share the module's zlib(-ng) abstraction —
  `http_compression_gzip_deflate_buffer` (in `http_compression_gzip.c`) and
  `http_compression_gzip_inflate_buffer` (factored out of the request-body
  `decode_gzip`, which now calls it). Declared in
  `include/compression/http_compression_message.h`. gRPC calls them via
  `grpc_message_inflate` / `grpc_message_deflate_gzip`.
  `readMessage()` transparently inflates a message whose compressed flag is
  set (per `grpc-encoding`, gzip only); `writeMessage($msg, compress: true)`
  gzips the reply and sets the `grpc-encoding: gzip` response header. Gated on
  `HAVE_HTTP_COMPRESSION` — identity-only when no zlib backend is built. Note:
  gRPC compression is **per-message** (a flag byte per message) and is
  distinct from HTTP-body `Content-Encoding`, which is why it needs its own
  path rather than the streaming HTTP-body encoder. Test: `/008` (10 KB gzip
  round-trip both directions).
- **Phase 5a — DONE (grpc-web, binary).** grpc-web carries the trailers inside
  the response body as a `0x80`-flagged frame (browsers can't read HTTP/2
  trailers). Same `addGrpcHandler` / `readMessage` / `writeMessage` API — only
  the finalize differs: `grpc_request_is_grpc_web` (content-type
  `application/grpc-web…`) sets `stream->grpc_web`; the response content-type
  becomes `application/grpc-web+proto`; and `h2_grpc_web_finalize`
  (`http2_strategy.c`) builds the trailer frame via `grpc_web_trailer_frame`,
  clears the HTTP trailers, appends the frame as the final DATA, and ends the
  stream with END_STREAM (no HTTP trailer). Works for streamed and
  zero-message replies. Tests: `/009` (binary round-trip + in-body trailers),
  `/010` (zero-message error). **Deferred (Phase 5c): grpc-web-text** (base64)
  — needs a stateful streaming base64 layer on request and response; binary
  grpc-web is the common case and is complete.

### Phase 5b — gRPC over HTTP/3 (feasibility confirmed; not yet built)

Feasibility mapped against the live tree + installed nghttp3 **1.15.0**. The
verdict is favorable but the work is the **largest single phase** and touches
the QUIC/nghttp3 internals, the reactor/worker path, and the C test client.

**Reuse (already works on H3):** the gRPC data plane is transport-agnostic —
`writeMessage()`/`readMessage()`, the framing/deframing codec, per-message
gzip, `grpc-timeout`, and every response-object trailer helper
(`ensure_grpc_status`, `promote_trailers_to_headers`, `clear_trailers`,
`grpc_web_trailer_frame`) are shared C. H3 installs the same
`http_response_install_stream_ops` seam (`http3_dispatch.c:366`) with its own
`h3_stream_ops`, so message streaming over H3 already functions.

**What must be built (H3-only):**
1. **Stream state** — add `is_grpc` / `grpc_web` / `has_trailers` /
   `trailers_submitted` to `http3_stream_t` (`include/http3/http3_stream.h`,
   near `streaming_ended:127`).
2. **Routing** — in `http3_stream_dispatch` (`http3_dispatch.c:282`): classify
   via `grpc_request_is_grpc`/`_web`, add `HTTP_PROTOCOL_GRPC` to the handler
   lookup (`:317-321`), relax the `fcall == NULL` bail (`:328`) for gRPC-only,
   inject the response content-type after `response_zv` init (`:360-367`).
   **Also the reactor path**: `http3_stream_dispatch_to_worker` (`:299`) →
   `worker_dispatch.c` needs the same gRPC routing for `REACTOR_POOL=1`.
3. **Trailer emission** — the real work, API available: change the streaming
   EOF branch of `h3_read_data_cb` (`http3_callbacks.c:476-479`) to set
   `NGHTTP3_DATA_FLAG_EOF | NGHTTP3_DATA_FLAG_NO_END_STREAM` and call
   `nghttp3_conn_submit_trailers(conn, stream_id, nv, nvlen)` when the response
   has trailers — mirroring `h2_dp_mark_eof`. Add an H3
   `submit_response_trailers` helper (QPACK-flatten via the existing
   `h3_nv_push`). Add the Trailers-Only / grpc-web finalize to
   `h3_handler_coroutine_dispose` (`http3_dispatch.c:680`), reusing
   `grpc_web_trailer_frame` + `clear_trailers` exactly as `h2_grpc_web_finalize`.
4. **Test client** — extend `tests/h3client/h3client.c`: inject arbitrary
   request headers (fixed 5-header set today at `:311`, so it can't send
   `content-type: application/grpc`) and record response trailer name/values
   (`h3_recv_header:113` discards non-`:status`). Then add
   `tests/phpt/server/h3/` gRPC tests.

**Why it's its own effort:** multi-file change across the trickiest subsystem
(QUIC + nghttp3 data-reader) plus the reactor/worker path plus a C QUIC-client
extension to make it testable. High reuse keeps it bounded, but it is not a
quick add — best done as a focused session, not rushed onto the end of the H2
work.

### Discovered pre-existing bug (independent of gRPC) — NOT yet fixed

An **uncaught exception in any HTTP/2 handler** (gRPC or plain HTTP, GET or
POST) leaks a reactor handle and aborts the worker at teardown
(`ZEND_ASYNC_REACTOR_LOOP_ALIVE() == false` assertion in ext/async
`scheduler.c`). Reproduced with a plain `addHttpHandler` that only does
`throw` — so it is not caused by the gRPC work and was never covered by a
test. The gRPC dispose path already maps an uncaught exception to
`grpc-status: 13` (INTERNAL), but that mapping cannot be exercised until this
teardown leak is fixed. **Recommend a separate fix/issue** for the h2
handler-exception teardown; realistic gRPC handlers should catch their own
errors and set `grpc-status` explicitly (works today — see test 002).

gRPC server support on top of the **existing** HTTP/2 (and, later, HTTP/3)
stack. Framing + trailers + status + deadline are implemented **in C**;
protobuf stays entirely in PHP userland (`ext/protobuf`). The C layer only
ever moves opaque, length-prefixed message octets.

---

## 1. Context & the core decision

Issue #4 asks for gRPC (unary + all three streaming shapes) over HTTP/2 and
HTTP/3, with `grpc-status`/`grpc-message` trailers, `grpc-timeout` deadlines,
`application/grpc[+proto]` content-types, optional gzip and grpc-web, verified
against a `grpc-go` interop client.

**Decision: hand-write a thin gRPC framing layer directly on the existing
nghttp2 stack. Do _not_ vendor gRPC C-core (`libgrpc`).**

C-core is not a protocol helper — it is a complete stack that ships its own
HTTP/2 transport ("chttp2"), its own `EventEngine`/iomgr, and its own
completion-queue thread pool. All three collide head-on with what this server
already owns (nghttp2, the libuv reactor, the True Async coroutine scheduler),
and none of them expose an injection point that would let C-core consume an
already-decoded nghttp2 stream. Vendoring it would mean running two HTTP/2
stacks in one process and fighting a foreign threading model, plus a multi-MB
dependency tree (protobuf, Abseil, c-ares, re2, BoringSSL).

This is also the mainstream choice: `grpc-go` writes framing on
`golang.org/x/net/http2`, Rust `tonic` on the `h2` crate, nginx `grpc_pass`
by hand. C-core is the _only_ stack that bundles its own HTTP/2. There is no
standalone "gRPC wire-logic" C library to vendor — the layer is too thin to
warrant one (~350–500 LOC of core framing/status/routing).

Use `grpc-go`/`grpc-c++` **only** as an interop test peer, never as a linked
dependency.

---

## 2. What already exists (reuse map)

The server was scaffolded for gRPC. Almost the entire protocol surface is
present; the work is convention on top plus three wire fixes.

| Capability | Where | State |
|---|---|---|
| Per-stream → coroutine dispatch, multiplex-safe (`extended_data`=stream) | `http2_strategy.c:325` | ✅ reuse as-is |
| Dispatch at HEADERS+END_HEADERS **before** body (bidi enabler) | `http2_session.c:581` (spawn `:594`) | ✅ reuse as-is |
| Incremental request body (client-streaming ingest) | `cb_on_data_chunk_recv` `http2_session.c:397`; `http_body_stream` push/pop `src/http_body_stream.c:38/78`; `HttpRequest::readBody()` `src/http_request.c:606` | ✅ reuse |
| Incremental response DATA (server-streaming) | `submit_response_streaming` `http2_session.c:1784`; `chunk_queue` + deferred data-provider; `HttpResponse::send()/end()` `src/http_response.c:867/1062` | ✅ reuse |
| HTTP/2 trailer emission | `http2_session_submit_trailer` `http2_session.c:1863` + `h2_dp_mark_eof` (`NO_END_STREAM`) `:1472`; PHP `setTrailer/getTrailers` `src/http_response.c:442-540` | ✅ unary path; ⚠️ streaming gap (§4) |
| `:status`/header flatten via HPACK | `h2_flatten_response_headers`, `h2_nv_set_status` `http2_session.c:1664` | ✅ reuse |
| gRPC-style keepalive (PING RTT, graceful GOAWAY, MAX_CONNECTION_AGE) | `http2_session.c:653-693`; `http_server_config.c:1169` | ✅ reuse |
| `addGrpcHandler()` → `protocol_handlers[HTTP_PROTOCOL_GRPC]` | `src/http_server_class.c:1534` | 🟡 stores handler, otherwise inert |
| `HTTP_PROTOCOL_GRPC` enum + mask + name mapping | `http_protocol_strategy.h:126`; `http_protocol_handlers.c:25/46` | 🟡 defined, unrouted |
| Transport-agnostic PHP API shared with H3 | `src/http_response.c`, `src/http_request.c`, `src/http_body_stream.c` | ✅ codec written once serves H2 + H3 |

**Consequence:** all four RPC shapes are one duplex code path. Unary /
server-stream / client-stream / bidi are a userland contract, not a wire
signal — early dispatch + `readMessage()` + `writeMessage()` on the same
coroutine already give bidi.

---

## 3. Architecture of the thin layer

```
  HTTP/2 stream  ──dispatch──▶  is content-type application/grpc  &&  grpc handler set?
                                        │ yes                         │ no
                                        ▼                             ▼
                              mark stream->is_grpc,             normal HTTP handler
                              inject content-type,              (unchanged)
                              parse grpc-timeout → deadline
                                        │
                                        ▼
                       grpc handler coroutine (request, response)
                          reads:  $req->readMessage()   ← C deframer over request body
                          writes: $resp->writeMessage() → C framer → chunk_queue (streaming send)
                          status: $resp->setTrailer('grpc-status', N) (default 0 if unset)
                                        │ dispose
                                        ▼
                       grpc commit: ensure grpc-status trailer, emit
                       HEADERS(200, application/grpc) + DATA(msgs) + HEADERS(trailers, END_STREAM)
                       or Trailers-Only (single HEADERS, END_STREAM) when 0 messages
```

### New module: `src/grpc/`
- `grpc_frame.{c,h}` — 5-byte prefix framer (`flag u8 + len u32be + bytes`) and
  a deframer state machine that reassembles messages spanning DATA frames,
  with a hard `max_recv_message` guard (defends against a forged 4 GiB length).
- `grpc_status.h` — the 0–16 status enum + `grpc-message` percent-encoder
  (UTF-8 bytes ≤0x20, `%`, ≥0x7F → `%XX`).
- `grpc.{c,h}` — request classification (`grpc_request_is_grpc`), `grpc-timeout`
  parse, `:path` → (service, method) split, and the H2 commit/dispose glue.

### Stream-struct additions (`include/http2/http2_stream.h`)
- `bool is_grpc;`
- `zend_fcall_t *grpc_handler;` (per-stream handler override; NULL = use `conn->handler`)
- deframer cursor state for `readMessage()` (or hang it off the request object)
- deadline bookkeeping for `grpc-timeout`

### PHP API (minimal surface — 2 methods)
- `HttpRequest::readMessage(): ?string` — next deframed message (protobuf bytes),
  `null` when the client half-closed with no more messages. Loop for
  client-streaming; call once for unary.
- `HttpResponse::writeMessage(string $message): static` — frame + enqueue one
  message (drives the streaming `send()` path). Call N times for
  server-streaming; once for unary.
- Status stays on the existing trailer API: `setTrailer('grpc-status', '0')`
  (+ optional `grpc-message`). The C commit defaults `grpc-status: 0` when the
  handler set none, and auto-injects `content-type: application/grpc`.

**Unification:** every gRPC response rides the **streaming** path (even unary,
via `writeMessage`→`send`). A zero-message response is finalized by the same
`h2_stream_mark_ended` empty-commit that SSE already uses. This means the
streaming trailer fix (§4.1) is the _only_ trailer path gRPC needs — the
buffered path is never used for gRPC.

---

## 4. The three verified wire gaps (fix before/while building)

All three confirmed by direct code reading, not speculation.

### 4.1 Streaming responses never emit trailers — **blocker**
`h2_stream_mark_ended` (`http2_strategy.c:1653-1685`) only resumes + emits; it
never calls `http2_session_submit_trailer`. The unary path does
(`http2_strategy.c:1017-1046`), but streaming does not. Without this,
`grpc-status` after any server-streaming/bidi body is silently dropped and the
client hangs. **Fix (Phase 0):** before the final
`http2_session_resume_stream_data`, read `http_response_get_trailers` and
submit them, so `has_trailers` is set and `h2_dp_mark_eof` stamps
`NO_END_STREAM` on the last DATA slice. Generic, benefits every trailer user.

### 4.2 Empty-body responses drop trailers → gRPC errors break — **blocker**
`http2_session_submit_response` (`http2_session.c:1739-1743`) sends a `NULL`
data-provider when `body_len == 0` → HEADERS+END_STREAM, closing the stream
before the subsequent `submit_trailer` can run. gRPC errors (`UNIMPLEMENTED`,
`DEADLINE_EXCEEDED`, …) are exactly no-body responses. Because gRPC always uses
the streaming path (§3), the natural fix is: the zero-message gRPC dispose
force-activates the streaming empty-commit (already handled at
`http2_strategy.c:1669-1680`) so §4.1's trailer emission runs — yielding
HEADERS + empty DATA + trailing HEADERS, which `grpc-go` accepts. (True
single-HEADERS Trailers-Only via header-merge is an optional later
optimization.)

### 4.3 Body cap is a **lifetime** cap → long client/bidi streams get RST — **high**
`http2_session.c:458` gates each inbound chunk on
`body_bytes_consumed + body_bytes_queued + len > body_cap`, and
`body_bytes_consumed` (`http_body_stream.c:96`) is monotonic — never
decremented. So once a stream has _ever_ received `max_body_size` (default
10 MiB) it is RST, even if the handler drained everything. gRPC streams are
lifetime-unbounded by design; a long client-streaming/bidi RPC must be able to
push gigabytes. **Fix (Phase 2):** on gRPC streams, bound the _live_ (queued,
not cumulative) bytes and/or apply a per-message max instead of the cumulative
ceiling.

**Also:** never `RST_STREAM` on normal completion — a `NO_ERROR` RST arriving
before the client reads the trailing HEADERS can swallow `grpc-status: 0`
(Envoy #30149 / grpc-go #8041). Clean streams close via END_STREAM; RST only on
genuine cancel/abort.

---

## 5. Phases

Each phase must **build** and land **green phpt tests** before the next.

### Phase 0 — streaming trailer emission (prerequisite, protocol-agnostic)
- **Do:** §4.1 fix in `h2_stream_mark_ended`.
- **Test:** `tests/phpt/server/h2/` — a `send()`-streamed response that also
  `setTrailer('grpc-status','0')`; assert the trailing `grpc-status: 0` header
  appears after the body (curl `--http2-prior-knowledge -v`, mirror of
  `003-h2c-trailers.phpt`).
- **Exit:** trailers ride both unary and streaming responses.

### Phase 1 — gRPC unary over h2c/h2
- **Do:** `src/grpc/` module (framer/deframer + status + percent-encode);
  content-type routing at `http2_strategy_dispatch` (mirror the
  `h2_request_is_ws_connect` check); `stream->is_grpc` + handler override;
  `HttpRequest::readMessage()` / `HttpResponse::writeMessage()`; auto-inject
  `content-type: application/grpc` + default `grpc-status: 0`; wire the module
  into `config.m4` (new guarded block ≈`:570`), `config.w32` (`ADD_SOURCES`
  ≈`:154`), `CMakeLists.txt`.
- **Test:** new `tests/phpt/server/grpc/`. Client crafts a body =
  `\x00` + `uint32be(len)` + protobuf bytes (hand-built in PHP), POSTs via curl
  with `-H 'content-type: application/grpc' -H 'te: trailers'
  --http2-prior-knowledge --data-binary @body -o out.bin`; assert response
  status 200, `content-type: application/grpc`, `grpc-status: 0` trailer, and
  the echoed framed message bytes (`bin2hex` on `out.bin`). Error path:
  unknown method → `grpc-status: 12` Trailers-Only.
- **Exit:** unary round-trip + `UNIMPLEMENTED`/error path green.

### Phase 2 — streaming (server / client / bidi) + body-cap fix
- **Do:** message loop over the existing `chunk_queue` (out) and
  `http_body_stream` (in) for all four shapes on the one duplex path; §4.3
  body-cap fix. Half-close (END_STREAM → body EOF) already models
  client-done.
- **Test:** `H2TestClient` (`tests/phpt/server/h2/_h2_client.inc`) or curl —
  server-streaming (N frames then trailers), client-streaming (N request
  frames → single response), bidi; assert no RST race and trailers after the
  last message. A >10 MiB client-streaming test guards §4.3.
- **Exit:** all four shapes green.

### Phase 3 — `grpc-timeout` deadline
- **Do:** parse `<≤8 digits><H|M|S|m|u|n>` at dispatch; arm a per-stream
  deadline bound to the coroutine (reuse existing per-request timeout/cancel
  machinery); on fire, cancel the coroutine; inbound `RST_STREAM(CANCEL)` →
  cancel (path exists). Server need not emit `grpc-status 4` itself (the client
  enforces its own deadline) — scope this to parse + cancel.
- **Test:** slow handler + short `grpc-timeout`; assert the handler is
  cancelled and the stream closes.
- **Exit:** deadline cancels the handler coroutine.

### Phase 4 — per-message gzip (optional)
- **Do:** `grpc-encoding` / `grpc-accept-encoding` negotiation + Compressed-Flag
  byte via zlib (already linked); ship `identity` first, then gzip; unsupported
  request codec → `grpc-status 12` + advertise support.
- **Test:** gzip-flagged request/response round-trip.

### Phase 5 — grpc-web + HTTP/3 (optional, separate track)
- **grpc-web:** redirect trailer emission into an in-body `0x80`-flagged frame
  (+ base64 for `-text`, + CORS), behind a content-type switch. Shares the
  framer, **not** the HTTP/2 trailer machinery.
- **HTTP/3:** the H3 stack (ngtcp2+nghttp3, `src/http3/`) is mature for
  request/response but has **no trailer emission at all** — no
  `nghttp3_conn_submit_trailers`, no `NGHTTP3_DATA_FLAG_NO_END_STREAM`. That
  path must be built from scratch (unary _and_ streaming) before the same
  framing codec drops onto `src/http3/`. Roughly a second H2-sized effort;
  gate on `grpc-go`/H3 interop maturity.

---

## 6. Build & test commands

```bash
# after editing existing .c files:
make -j"$(nproc)"

# after adding a NEW .c file (config.m4 / config.w32 / CMakeLists touched):
phpize && ./configure --enable-http-server --enable-http2 --enable-http3 \
    --with-php-config="$(which php-config)" && make -j"$(nproc)"

# run one test (or a dir):
php run-tests.php -d extension_dir="$(pwd)/modules" -P -q \
    tests/phpt/server/grpc/
```

Interop (future): add `tests/phpt/server/grpc/` wire tests now; a `grpc-go`
interop harness can later mirror `tests/interop/quic/`'s Dockerized runner
pattern. Per-phase exit gate follows the #59 discipline (code-quality review,
no dead code, comments ≤1–2 lines, tests green incl. fuzz where relevant).

---

## 7. Risks

- **Trailers-Only vs two-HEADERS** and the **RST-vs-trailers race** are the
  classic interop-incompat bugs — test both explicitly (§4.2, §4.3 note).
- **PHP-side message API** must reassemble boundaries centrally in C (the
  deframer), never per-handler.
- **HTTP/3** trailers are a from-scratch build, not a codec drop-in — the H3
  half of the issue is a genuinely separate effort (Phase 5).
- Framing-in-C changes the byte contract of `send()`/`readBody()` **only on
  gRPC routes** — keep it from leaking into non-gRPC paths.
