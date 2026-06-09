# Reactor Thread Pool — Design Plan (#80 / #72 / #81)

Status: **design accepted, not yet implemented.** Master checklist for the pure-C
reactor-thread pool that decouples transport I/O from PHP business logic.

## Problem & framing

- **#80** (the driver): in QUIC/HTTP3 the *entire* transport — ACK generation, loss
  detection, RTT/PTO, pacing, idle timers — runs in userspace on our reactor loop
  (ngtcp2 has no internal threads). A synchronous CPU/blocking burst in a per-request
  PHP handler on that thread delays ACKs → inflates peer RTT/PTO, stalls cwnd, drops
  connections, and reintroduces head-of-line blocking *at the transport layer*. Budget:
  reactor iterations must stay single-digit-ms (< `max_ack_delay` 25 ms).
  - **This is QUIC-specific.** On TCP the kernel ACKs independently → no transport-stall.
    So the reactor pool's primary client is **H3**; H1/H2 reuse is future and optional.
- **#72** (rides along): with `setWorkers > 1` the `SO_REUSEPORT` rehash routes a
  migrated QUIC 4-tuple to a worker that doesn't own the connection → stateless reset.
  Needs a worker-id encoded in the CID. See `docs/H3_ROADMAP.md` §"Cross-worker steering".
- **#81** (the foundation, already merged on this branch): lock-free bounded MPSC/SPSC
  queues + reactor-integrated mailbox (`include/core/thread_queue.h`,
  `include/core/thread_mailbox.h`). Currently **unwired** — only a unit test. This is the
  non-blocking handoff primitive the pool is built on.

## Core architecture — two tiers, opposite scheduling policies

A pool of **pure-C reactor threads** (no PHP *executes*) owns all transport; a pool of
**PHP worker threads** runs handlers. Bridged by the #81 queue. The two tiers get
deliberately opposite policies:

| | Transport (sockets, ngtcp2, TLS, ACK/timers) | Business logic (PHP handler) |
|---|---|---|
| Policy | share-nothing, **pinned**, connection never migrates | load-balanced dispatch |
| Why | per-conn crypto/congestion/PN state is a hot working set; migrating it = cache-miss avalanche (Seastar) | a finished request is position-independent |

H3 request data-flow under the split:

```
datagram ─(SO_REUSEPORT │ eBPF-CID-steer)─▶ Reactor R (pinned, 0 PHP)
  R: ngtcp2_read_pkt → decrypt → reassemble HEADERS+DATA
     on end_stream: build FLAT request_wire {method,:path,header spans,body buf,
                                              reactor_id=R, stream, conn}
     pick worker W (sticky default + threshold + P2C — see §Dispatch)
     post(request_wire) → W.mailbox (#81)
     ─▶ R immediately resumes ACK/timers/other conns — transport NEVER blocked
Worker W (php-async thread, has interpreter):
  build $request zval from request_wire ON ITS OWN thread → run handler coroutine
  (await DB/io on W's own reactor)
  render response into a PERSISTENT buffer {ptr,len,free_fn} → post back to R.mailbox
Reactor R:
  QPACK/HPACK-encode response headers + flow control + TLS encrypt + sendmsg
  slow client → out_buffer + re-arm write + watermarks (W already free, never blocks)
  streaming → W posts body chunks incrementally; bounded mailbox = backpressure
```

## Decisions (accepted)

### D1 — Thread substrate: ThreadPool + `submit_internal` (option B now, C later)

A "pure C, no TSRM" thread + the TrueAsync reactor is **impossible as built**: the reactor
lives in thread-local `ASYNC_G(uvloop)` (`ext/async/libuv_reactor.c:315`, `uv_loop_init`
in `libuv_reactor_startup` `:391`), touches `EG(exception)` throughout, and every pool
thread boots via `ts_resource(0)` (`ext/async/thread.c:1905`) + `php_request_startup()`
(`:1980`). No TSRM → no `ASYNC_G` → no `libuv_reactor` → no #81 mailbox (it registers a
`zend_async_trigger_event_t` on `ASYNC_G(uvloop)`). **#81 already assumes the consumer is
a Zend thread.**

- **B (now):** a reactor thread is a ThreadPool worker that runs a **C reactor loop via
  `submit_internal`** (`ext/async/thread_pool.c:813`, the `TASK_KIND_INTERNAL` path —
  this is literally how the server already boots workers: `pool->submit_internal(pool,
  pool_worker_handler, …)`) and **never enqueues a local handler coroutine** — it posts to
  workers. PHP does not execute on it. `libuv_reactor_execute()` (`:476`) is already
  separable from the coroutine scheduler. Zero php-src changes. **#81 mailbox works in
  BOTH directions** (R→W and W→R) because both pools are Zend threads with `ASYNC_G` — no
  raw-uv twin needed. Cost: a "sleeping" TSRM/interpreter per reactor (memory/init, not
  runtime).
- **C (later, php-async feature):** a lightweight reactor-only thread mode — `ts_resource(0)`
  + `libuv_reactor_startup()` **without** `php_request_startup()` and without scheduler
  bring-up; EG allocated-but-inactive (the reactor's `EG(exception)` touches are NULL-checks
  that stay false). Genuinely light "C-ish" reactor thread, still speaks ZEND_ASYNC + #81.
  Needs a spike to confirm nothing in ngtcp2 binding / `libuv_reactor_startup` requires an
  *active* executor.
- **B and C share the identical C reactor-loop handler** — only the thread-boot weight
  differs. Ship B, promote to C transparently if the sleeping TSRM proves costly.

### D2 — Request boundary: wire-parse on reactor, zval on worker, body streamed

The current `http_request_t` (`include/http1/http_parser.h:83`) is **not** thread-clean —
it's built from `zend_string*` / `HashTable*` (per-thread ZMM). The reactor (no usable ZMM)
cannot fill it. So:

- Reactor does **only wire-parse** of headers (HPACK/QPACK/llhttp — stateful per-conn,
  must be on the reactor anyway) and produces a **new flat `request_wire`**: raw
  `{ptr,len}` spans over its recv buffer (method, :path, header name/value spans) + body
  buffer + `{reactor_id, stream, conn}`. Zero PHP allocation on the reactor.
- Worker materializes `$request` zval (`zend_string_init`/`zend_hash`) **on its own thread**.
- **Body is streamed incrementally**, not buffered before handoff — maps onto the existing
  `readBody()` streaming path (#26): `body_event` + chunk feed.

### D3 — Response boundary: persistent buffer, reactor owns encode/flow/TLS

- Worker renders the response body into a **persistent / malloc-domain buffer**, passed as
  a generic descriptor `{char* data, size_t len, void(*free_fn)(void*)}`. **Required, not
  just convenient:** on worker graceful-shutdown the ZMM arena is destroyed, so an
  `emalloc` buffer the reactor is still draining would dangle. `free_fn` may be `pefree`
  (persistent `zend_string`) or a shared-slab-pool return (reuse, no malloc/free per
  response). Keeps the reactor **Zend-free**.
- **Ownership transfers at `post`** — that is the responsibility boundary. After post, the
  buffer is the reactor's; worker death no longer matters for it.
- Reactor does QPACK/HPACK response-header encode (stateful per-conn), stream+conn flow
  control, TLS encrypt, sendmsg. Slow client absorbed by reactor `out_buffer` + writable
  re-arm + high/low watermarks (Swoole model) — the worker never blocks on the client.
- **Static responses bypass PHP entirely** (already served in reactor/scheduler context via
  `http_static_try_serve`).

### D4 — Robustness model: graceful-only, thread-based

Workers die only **cooperatively** (exception / OOM → `ZEND_ASYNC_SHUTDOWN`, already caught
by the OOM-firewalls), never "suddenly". A true crash takes the whole process anyway
(thread model). So no shm/processes needed. Cleanup = persistent buffer (D3) + worker
**shutdown-hook** (free not-yet-posted buffers via a per-request registry; signal the
reactor to `RST_STREAM`/500 orphaned streams) + a reactor **liveness-timeout** on streams
awaiting a response that never comes.

### D5 — Dispatch: sticky default + threshold-on-dispatch + P2C re-selection

Per-worker **MPSC mailbox** (one consumer each — #81 as-built), **not** one shared MPMC
queue (global head/tail contention + no affinity + breaks #81's single-consumer
edge-triggered wakeup). The reactor picks *which* mailbox:

- Per worker: `_Atomic uint32_t outstanding` (cache-line padded), `+1` by reactor at
  dispatch, `−1` by worker at completion = "queued + in-flight" in one number.
- **Hot path:** push to `reactor->default_worker` (sticky → max locality, warm
  zval/parser caches, zero sampling).
- **Trigger (free):** `atomic_fetch_add` *returns* the prior count → one compare, no extra
  read. If `prev >= HIGH`, the default is loaded → **re-select via P2C** (sample 2, take
  the lighter), set it as the new sticky default.
- **Self-damping / auto-interpolating:** the P2C re-pick lands on a lighter worker, so the
  next dispatch is sticky again. Under global saturation (all ≥ HIGH) it naturally
  degrades to per-request P2C — which is optimal there. Cost: **≤ P2C always, strictly
  cheaper when there's slack.** No timer.
- Memory ordering: `relaxed` everywhere (heuristic; the mailbox provides the real
  happens-before). Signal is `outstanding` (reactor-observable at decision time), **not**
  CoDel-sojourn (computed later on the worker — it stays for admission/shedding).

Why this and not the alternatives (researched against HAProxy/Nginx/Envoy/Finagle/Linkerd
+ Mitzenmacher/JIQ/Tokio/Go): plain P2C-over-outstanding is the industry default and
near-optimal at saturation; JSQ full-scan = O(N) cache-bounce for a vanishing balance gain;
JIQ collapses to *random* (worse than P2C) under high load unless patched to P2C-fallback;
peak-EWMA ≡ P2C for homogeneous local workers; one shared MPMC queue = global contention +
no affinity. Work-stealing ("воровство") is **obviated** by smart-push: we have cheap
centralized load info, and #81's MPSC-per-worker design would have to become MPMC to allow
stealing.

### D6 — H3 CID steering (closes #72)

Today the SCID is 8 random bytes (`src/http3/http3_connection.c:301`,
`http3_fill_random(c->scid, HTTP3_SCID_LEN)`); `routing_dcid` already exists (`:483`).

- **Encode reactor-id into the SCID** (reserve 1–2 high bytes, obfuscated QUIC-LB-style:
  AES-ECB single block / 3-pass Feistel; rest random). We mint the SCID the client echoes
  as DCID → constant for the connection's life.
- **Two-level routing (h2o model):** short-header (established) → read reactor-id from
  DCID, mine→process else→forward to owner; long-header Initial (client-chosen DCID, no id)
  → 4-tuple hash / kernel SO_REUSEPORT.
- **Forward channel: userspace fan-out via #81 first** (in-process, same address space —
  cheaper than h2o's AF_UNIX socketpair; works everywhere incl. WSL/Windows). eBPF
  `SO_ATTACH_REUSEPORT_EBPF` is a later opt-in optimization (no userspace hop, immune to
  reuseport reshuffle — but needs CAP_BPF, no WSL/Windows). **TTL** on forwards bounds
  ping-pong. Fixes migration/NAT-rebind for free (CID constant).

## Phased roadmap

- **Phase 0 — instrument #80 (cheap, self-contained, tells us if/where offload is needed). DONE.**
  - [x] Reactor-iteration latency histogram + "time since last ACK per connection";
        watchdog logs when an iteration exceeds the budget. — `reactor_*` counters in
        `http3_packet_stats_t`, timed in `http3_listener_poll_cb` (tick latency) and
        `timer_fire_cb` (per-conn timer-fire lateness = ACK/PTO service delay), surfaced
        via `HttpServer::getStats()`, rate-limited `WARN h3.reactor.slow_tick`. Budget
        `PHP_HTTP3_REACTOR_BUDGET_MS` (default 10 ms).
  - [x] Audit the H3 request path for synchronous CPU bursts without a yield (heavy
        render/serialize, sync DB, large gzip, big `smart_str`). — `docs/PHASE0_H3_REACTOR_AUDIT.md`.
        Headline: buffered-response compression in `http3_stream_submit_response` is the
        one unbounded synchronous span (runs in dispose context) → primary Phase 1 offload target.
  - [x] CODING_STANDARDS rule: the transport reactor stays strictly non-blocking. —
        `docs/CODING_STANDARDS.md` §1.5.
- **Phase 1 — validate the marshalling boundary on a narrow surface (nginx model).**
  - [ ] Wire #81 to offload only heavy ops (large gzip/brotli, sync-CPU) to a small pool.
        Validates MPSC + the C↔bytes serialization on 1–2 ops before the full pipeline.
- **Phase 2 — full transport/worker split for H3.**
  - [ ] Reactor pool via ThreadPool + `submit_internal` C loop (D1/B).
  - [ ] `request_wire` flat type + worker-side zval materialization (D2).
  - [ ] Persistent response buffer + ownership transfer + reactor encode/TLS (D3).
  - [ ] Sticky-default + threshold + P2C dispatch (D5).
  - [ ] CID-in-SCID steering + userspace fan-out (D6); closes #72.
  - [ ] Worker shutdown-hook + reactor liveness-timeout (D4).
- **Phase 3 — H1/H2 into the pool — ONLY if Phase 0/2 measurements justify it.**
  No transport-stall there; the win is unified architecture / blocking-handler offload, not
  ACK timing. Default: leave H1/H2 on the current share-nothing same-thread model.

## Open items / deferred decisions

- `HIGH` threshold value for D5 (tune; ~2–4× steady-state per-worker depth).
- Reactor count vs worker count topology (R:W ratio; pinning policy).
- Option C spike (lightweight reactor-only thread mode in php-async).
- eBPF CID steering (Phase 2+ opt-in).
- Idle-fast-path (JIQ-Pod) — only if the load profile turns out bursty rather than steadily
  hot; degrades cleanly to D5's P2C, so additive later.

## Prior-art references

- **h2o** — share-nothing thread-per-core; CID-encoded thread/node id + AF_UNIX socketpair
  forward + TTL; mutex-MPSC mailbox with eventfd edge-triggered wakeup + whole-batch drain.
- **Swoole** — reactor threads (0 PHP) + worker pool; `dispatch_mode` menu; idle/busy =
  self-updated status byte; `session→{fd,reactor_id}` for the send-back path; reactor owns
  out_buffer + watermarks.
- **Seastar / Tokio / Go** — share-nothing + work-stealing on the pull side; informs why we
  do smart-push instead.
- Load-balancing theory — Mitzenmacher P2C (`log log n`), JIQ (Lu et al.), Envoy/Nginx/
  HAProxy/Finagle/Linkerd P2C-in-production.
