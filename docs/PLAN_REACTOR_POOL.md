# Reactor Thread Pool — Design Plan (#80 / #72 / #81)

Status: **design accepted, not yet implemented.** Design plan for the pure-C
reactor-thread pool that decouples transport I/O from PHP business logic.

> **Revision 2026-06-14 — request boundary reversed.** The copy-marshal request
> wire (D2) is superseded by **actor-model handoff: one `http_request_t`, one
> parser path, the request crosses the thread boundary by pointer.** See **D7**
> (request ownership) and **D8** (reverse path: bidirectional cancel,
> validate-and-drop, generationed handle). D2 is kept below struck-through for
> history. `response_wire` (D3) is unaffected and stays.

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

### D1 — Thread substrate: ThreadPool + `submit_internal`

A "pure C, no TSRM" thread + the TrueAsync reactor is **impossible as built**: the reactor
lives in thread-local `ASYNC_G(uvloop)` (`ext/async/libuv_reactor.c:315`, `uv_loop_init`
in `libuv_reactor_startup` `:391`), touches `EG(exception)` throughout, and every pool
thread boots via `ts_resource(0)` (`ext/async/thread.c:1905`) + `php_request_startup()`
(`:1980`). No TSRM → no `ASYNC_G` → no `libuv_reactor` → no #81 mailbox (it registers a
`zend_async_trigger_event_t` on `ASYNC_G(uvloop)`). **#81 already assumes the consumer is
a Zend thread.**

A reactor thread is a ThreadPool worker that runs a **C reactor loop via
`submit_internal`** (`ext/async/thread_pool.c:813`, the `TASK_KIND_INTERNAL` path —
this is literally how the server already boots workers: `pool->submit_internal(pool,
pool_worker_handler, …)`) and **never enqueues a local handler coroutine** — it posts to
workers. PHP does not execute on it. `libuv_reactor_execute()` (`:476`) is already
separable from the coroutine scheduler. Zero php-src changes. **#81 mailbox works in
BOTH directions** (R→W and W→R) because both pools are Zend threads with `ASYNC_G` — no
raw-uv twin needed. Cost: a "sleeping" TSRM/interpreter per reactor (memory/init, not
runtime) — accepted.

### D2 — Request boundary: wire-parse on reactor, zval on worker, body streamed

> **⚠ SUPERSEDED by D7 (2026-06-14).** The flat `request_wire` copy-marshal is
> replaced by actor handoff-by-pointer (one struct, one parser). `request_wire` +
> `http_request_from_wire` are deleted for the request path. The body-streaming
> insight below survives, but evolves into D7's worker-applied command stream
> (append-chunk / body-complete / release) instead of a chunk-feed the reactor
> writes. Text kept for history.

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

> **Refined by D8 (2026-06-14).** Mid-flight cancellation is now **bidirectional**
> (client-gone → reactor tells worker; handler-dead → worker tells reactor), and
> the reactor-side liveness-timeout is largely subsumed by **validate-and-drop**:
> the reactor frees stream state on the normal QUIC lifecycle and silently drops
> late worker messages for streams that are already gone, instead of holding them
> alive on a timeout.

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

**Scope.** Steering is only exercised on **migration / NAT-rebind** — in steady state the
4-tuple is stable, so `SO_REUSEPORT` keeps delivering a connection's packets to its owner
and nothing is forwarded. It is not a hot path.

**Why forward at all (not migrate the connection).** The reactor owns the connection
memory — `ngtcp2_conn`, crypto keys, stream-reassembly buffers — and a raw QUIC packet is
encrypted, so only the owner can read it. Moving live conn+crypto state across threads on
every rebind is the racy path we reject; forwarding ~1 datagram is cheaper. **Rule: the
packet goes to the memory, not the memory to the packet.**

**Forward carries the DCID, not a conn pointer.** The owner re-looks-up by DCID and a
lookup miss is just a drop (ngtcp2 already drops unknown CIDs). No dangling pointer crosses
threads → steering is **independent of D4/D8's generationed handle** (that one is for the
request/stream pointer on the reactor↔worker axis, a different axis).

**Decided defaults (2026-06-19):**
- **Copy the datagram on forward.** Rare path, datagram ≤ MTU → the `memcpy` is in the
  noise, and it sidesteps changing the hot recv path. Zero-copy (recv into owned slab
  buffers + return via `reactor_pool_post_exec` reclaim, the existing D7.5 pattern) is a
  later optimization, only if forwarding ever turns hot.
- **Obfuscate the reactor-id** (QUIC-LB AES-ECB single block / Feistel) before production.
  A plaintext id may ship first under the dev gate; it must not reach prod (leaks topology
  → targeted single-thread DoS).

**Build order (D6):**
1. Encode reactor-id into the minted SCID + a decode helper from DCID
   (`http3_connection.c:301`; `routing_dcid` read side at `:483`).
2. Reactor registry (`id → datagram-inbox`) + per-reactor datagram-inbox — mirror of
   `worker_registry` / `worker_inbox`; wake via the existing trigger-event.
3. Classify on recv (long/short header bit → decode id → local vs `post` to owner) and a
   single `feed_datagram(buf, len, remote_addr)` called from both the socket loop and the
   inbox drain. Copy into the inbox message; TTL byte to bound bounce.
4. Test single-host migration with h3client's `MIGRATE_AFTER` (from #59).
5. (later, opt-in) eBPF reuseport steering to drop the userspace hop on Linux prod.

**Status — SHIPPED 2026-06-19 (steps 1–4).** Implemented as built: `http3_steer.c`
(AES-128 keystream over the CID nonce masks a 1-byte reactor id — obfuscated, not
plaintext), encode at SCID mint + in `get_new_connection_id_cb`, decode only on a
conn_map **miss** (off the hot path; the miss path already pays a stateless-reset HMAC).
The forward is a per-endpoint `http3_steer_group_t` (atomic `id → listener` table) +
`reactor_pool_post_exec` — **no new mailbox** (the reverse-path primitive already exists).
Active only with >1 reactor; gated behind `TRUE_ASYNC_SERVER_REACTOR_POOL`. Coverage:
`HTTP3Steer` unit test (deterministic encode/decode round-trip, masking, valgrind-clean) +
phpt `040` (a NAT-rebound connection is served across reactors with `setWorkers(2)` —
the regression 032 documents as broken). Suite green (H3 + reactor_pool 48/48).

**KNOWN LIMITATION — investigated deeply (server + client qlog), root is a circular
path-validation deadlock; fix is a deliberate QUIC effort.** Under **back-to-back**
migrations a connection intermittently stalls: ~5 % at 15 rebinds on one connection,
scaling to 0 % at ≤2–3 rebinds and **0/40 at a single realistic NAT-rebind**. It is a
**circular QUIC path-validation convergence deadlock** exposed only when migrated packets
traverse the cross-reactor forward under a pathological rebind rate. The loop:

1. Per RFC 9000 §9.3 the server (ngtcp2) keeps sending non-probing frames (the HTTP
   response + ACKs) on the **old, already-validated** path until the new path validates.
2. The client has moved to the new path, so it never receives those ACKs → its RTT stays 0
   and it **congestion-window-blocks** (server+client qlog: `bytes_in_flight 3600 > cwnd
   3538`, `smoothed_rtt 0`).
3. cwnd-blocked, the client cannot send the request retransmit / the final PATH_RESPONSE →
   the new path never finishes validating → back to step 1.

**Ruled out by data (each was a hypothesis that the evidence killed):**
- *RTT inflation* — refuted: `smoothed_rtt ≈ 0.2 ms` at the stall (normal localhost).
- *Forward-hop latency* — refuted: measured forward latency is **microseconds** (max
  ~0.17 ms even in stalls), nowhere near the tens-of-ms path-validation deadline.
- *Validation failure* — refuted: ngtcp2 `path_validation` callback reports **FAILURE=0**;
  validations succeed or are superseded (ABORTED), never fail. Full-success runs occur with
  **0** completed validations (anti-amplification covers the small response).
- *Our forward dropping packets* — refuted: `steered_in == steered_out`, 0 drops; the
  forward does not lose datagrams.
- *Addressing / steering* — correct: every migrated datagram decodes to the right owner
  (conn-map HIT). *Memory* — valgrind clean, no UAF/leak.
- *Test-client stale `c.local`* — that is deliberate NAT-rebind simulation; "fixing" it
  breaks h3client immediately (it has no client-initiated migration). Not the cause.

It is **forward-specific**: single-reactor mode (no hop — everything serialized in one
reactor tick) is **0 stalls** at the identical 15× migration load. The exact single mis-
pathed packet was **not** isolatable: the failure is circular, qlog carries no per-packet
addresses, every instrumentation perturbs the Heisenbug, and no independent H3 client
exists (host curl lacks HTTP/3; a QUIC client cannot be written "simply"). Trigger is
pathological (7+ rebinds/conn in milliseconds); real clients rebind occasionally and are
unaffected.

**Fix (separate task): eBPF `SO_ATTACH_REUSEPORT_EBPF`** — the kernel reads the DCID and
delivers the migrated datagram **straight to the owner's socket**, so there is no forward
hop and the connection behaves exactly like single-reactor (which never stalls). This is
the nginx approach (eBPF worker-socket map keyed on DCID). Linux + CAP_BPF only, hence it
stays the opt-in optimization over the portable userspace forward.

### D7 — Request ownership: actor handoff by pointer (supersedes D2) [2026-06-14]

**Decision reversed.** D2 marshalled the request through a flat `request_wire`
(copy on the reactor → re-materialize on the worker). Rejected as built. Replaced
by ownership handoff: **one struct, one parser path, the request crosses the
thread boundary by pointer.**

Rationale: H1/H2/H3 already build a single `http_request_t` *directly* via their
parser callbacks (`http3_callbacks.c:271,130`; `http2_session.c:291,129`;
`http_parser.c:205`). `request_wire_create` is called **only from
`reactor_pool_test.c`** — never in production. Pushing the wire into production
would force every protocol callback to grow a *second* emit target (build wire
alongside `http_request_t`), duplicating per-protocol method/header/body logic.
The goal is the opposite: parser code **almost identical** for single-thread and
split modes — `http_request_t` must not depend on the delivery mechanism.

1. **One struct, one parser.** `http_request_t` stays the sole request
   representation. The reactor fills it through the existing parser callbacks.
   `request_wire` + `http_request_from_wire` are **deleted** for the request path.

2. **Allocation domain from execution context, not a new field.** The only
   per-site delta is the persistent flag on `zend_string_init(v, len, persistent)`
   / hashtable init: the reactor has no usable ZMM → `persistent=1`; a
   single-thread worker → `0` (ZMM, fast). That bit comes from the parser's
   already-threaded context object (session / stream / parser), **not** a new
   `bool` on `http_request_t`. (Self-rejected: a struct flag is redundant.)

3. **Accessors self-describe on read.** `zend_string` already carries
   `IS_STR_PERSISTENT`. Each accessor inspects the string it returns: persistent
   → deep-copy into ZMM; ZMM → `addref` (`RETURN_STR_COPY`, unchanged). `getHeaders`
   (`http_request.c:199`) must **not** `zend_array_dup` a persistent HT — dup
   `addref`s persistent strings → VM `efree` → heap corruption; persistent mode
   needs a deep ZMM rebuild of the table.

4. **Handoff by pointer = the responsibility boundary.** The reactor builds the
   `http_request_t`, posts the **pointer** to the worker mailbox, and
   relinquishes the right to touch that memory. The worker is then **sole owner
   and sole writer**; the address is the identity/handle. No serialize, no
   re-materialize, zero copies.

   **Zero-alloc: the request stays in the reactor's stream slab [revised 2026-06-14].**
   For H3 the request is *embedded* in the pooled `http3_stream_t`
   (`s->_request_storage`, offset-0; `http3_stream.c:40`) — the per-listener slab
   already gives us a request slot with no `malloc`. The handoff therefore costs
   **zero allocations**: the reactor fills the embedded request (persistent
   strings) and hands its pointer over; nothing is cloned and nothing is
   separately `pemalloc`'d. (An earlier sketch proposed a standalone `pemalloc`
   request per handoff — *rejected*: the slab already does the job, see point 5
   for how the slot returns home without a cross-thread pool free.)

5. **Single-writer ⇒ non-atomic refcount; the slab slot is reclaimed by the
   reactor on command [revised 2026-06-14].** The existing `unsigned refcount`
   (`http_parser.h`) stays non-atomic and **only the worker mutates it** (the
   reactor never touches it during the borrow). The catch: the request lives in a
   reactor-owned slab slot, and that slot's `release` callback
   (`http3_stream_release_via_request` → `http3_stream_pool_free`) returns the slot
   to the **reactor's** per-listener pool — a pool with no locks, sized for one
   thread. So the worker must **not** invoke `release` itself: that would be a
   cross-thread slab free (two threads mutating one lock-free pool = corruption).

   Instead the free is *deferred to the owner thread*: when the worker's
   `--refcount` hits 0 it does **not** call `release`; it posts a **`consumed`**
   message (D8 reverse channel) and the **reactor** invokes `release` on its own
   thread, returning the slot to the pool. Worker mutates the refcount; reactor
   reclaims the slab. No clone, no standalone alloc, no cross-thread pool access.
   (For the self-test synthetic requests — `selftest_build_request` — the request
   is a standalone `pecalloc` with `release == NULL`, so the worker `pefree`s it
   directly; only the real H3 slab-backed request needs the `consumed` round-trip,
   which lands with the reverse channel in B4.)

6. **Post-handoff data = commands, applied by the worker.** For streaming bodies
   the reactor never writes into the worker's struct. It sends commands over the
   mailbox — `append-chunk`, `body-complete`, `release` — and the **worker**
   applies them in its loop (append to `body_queue_*`, notify `body_data_event` on
   its **own** thread — no cross-thread wake). Commands are FIFO per stream (one
   reactor producer); `release` is the last command. Free happens only after
   draining to `release` at refcount 0 → no UAF, no pointer-reuse ABA.

### D8 — Reverse path: bidirectional cancel, validate-and-drop, generationed handle [2026-06-14]

7. **One reverse channel per reactor, tagged messages, non-blocking post.**
   Worker→reactor carries `response` (render result), `consumed` (request done →
   the reactor invokes the slab `release` and reclaims the request slot, per D7.5;
   also flow-control replenish) and `cancel-stream`. One drain point per reactor; a
   tagged union gives `response`-then-`cancel` ordering for free. A dedicated control channel is
   **deferred** until profiling shows control-message starvation; if ever split,
   the line is data-vs-control (cancel + consumed together), not cancel alone. The
   two-bounded-queues deadlock is prevented **not** by channel count but by the
   rule that *neither side ever blocks on a full queue* — the reactor backpressures
   the **client** (stop reading the stream / shrink the FC window), never blocks on
   a worker mailbox.

8. **Reverse addressing: array of reactor channels indexed by `reactor_id`.** The
   routing triple `{reactor_id, stream_id, conn}` already exists
   (`request_wire.c:29-31`) and is echoed on the response. It must be carried
   **into `http_request_t`** (today `http_request_from_wire` drops it).
   `reactor_id` → which reverse channel; `stream_id` (+ conn handle) → which stream.

9. **Bidirectional cancel; nobody waits.**
   - **Client gone** (RST/close) → reactor sends the worker a `cancel` command →
     worker stops, cancels the handler coroutine (`ZEND_ASYNC_CANCEL` via the
     request's `coroutine` field), releases the request.
   - **Handler died** on the worker before body-complete (returned / threw /
     cancelled) → worker sends the reactor a `cancel-stream` message → reactor
     stops streaming and RST/closes the stream.
   - Neither side blocks. `cancel` means "begin teardown", not "stop now":
     already-posted commands drain/discard until `release`, which stays the
     terminator.

10. **Validate-and-drop on the reverse path; hold-alive rejected.** The reactor
    frees stream state on the **normal QUIC lifecycle** (client RST / completion),
    independent of worker timing. A late worker message for a gone stream → lookup
    fails → silently dropped. Hold-alive (reactor keeps the stream alive until the
    worker acks) is rejected: a client that opens + RSTs many streams would pin
    reactor memory **proportional to handler latency** — a DoS that violates #80's
    transport budget.

11. **Reverse identity = generationed handle, not a raw `conn` pointer.** A freed
    conn cannot be safely dereferenced. The reverse path replaces the raw
    `void *conn` (`request_wire.c:31`) with `(conn_id, conn_gen)` + `stream_id`; a
    reused slot is caught by a generation mismatch on lookup. The **forward**
    request pointer stays raw — safe because the worker is its sole owner/writer
    for the whole borrow (single-ownership + FIFO + FIN); the slab slot is only
    reclaimed after the worker's `consumed` (D7.5), so the pointer can never be
    reused under the worker. Asymmetry: forward memory's lifetime is driven by the
    message *consumer* (worker, via `consumed`) → raw pointer safe; reverse memory
    is owned by the reactor but its lifetime is driven by the **client**
    (unsolicited RST), not by the message sender → raw pointer unsafe.

## Build order

Reactor-tick + ACK/PTO-late watchdog instrumentation is already in place (`5884e2a`) —
the empirical check that the transport reactor stays inside the ACK budget.

The split itself, per the Decisions above (each maps to a D-item):

- [x] Reactor pool via ThreadPool + `submit_internal` C loop (D1/B).
- [x] ~~`request_wire` flat type + worker-side zval materialization (D2).~~ **superseded by D7.**
- [x] Persistent `http_request_t` build flag from execution context + self-describing
      accessors + persistent-aware `getHeaders` rebuild; **deleted `request_wire` +
      `http_request_from_wire`** for the request path (D7).
- [x] Command stream over the mailbox: handoff-pointer / append-chunk / body-complete /
      release; worker-applied refcount decref → reactor reclaims the slab slot on
      `consumed` (D7.5–6).
- [x] Reverse channel (response / consumed) per reactor + non-blocking post (D8).
- [ ] Bidirectional cancel + validate-and-drop with generationed conn handle (D8/D4) —
      deferred; the raw stream pointer is currently proven safe via the worker-borrow
      ref + reactor-thread serialisation, so the generationed handle is not yet needed.
- [x] Persistent response buffer + ownership transfer + reactor encode/TLS (D3).
- [x] **Dispatch policy (D5) — reactor-paired connection→worker affinity.** Each reactor
      owns a strided subset of workers ({i : i % n_reactors == reactor_id}); a connection
      homes to the least-loaded owned worker (idle ties rotate so connections spread) and
      reuses it for all its streams (`http3_connection.worker_slot` + `worker_registry_at`).
      A home backed up past `H3_WORKER_SPILL_DEPTH` spills the request to a less-loaded
      worker (owned first, then any reactor's); a home whose worker died is re-homed.
      `worker_registry_least_busy` is the primitive — unit-tested by reactor_pool/010,
      e2e by h3/037-041.
- [x] CID-in-SCID steering + userspace fan-out (D6); closes #72.
- [ ] Worker shutdown-hook (D4).

H1/H2 into the pool is future and optional: the kernel ACKs TCP independently, so there
is no transport-stall to fix there. Leave H1/H2 on the current share-nothing same-thread
model unless measurements say otherwise.

## Open items / deferred decisions

- `HIGH` threshold value for D5 (tune; ~2–4× steady-state per-worker depth).
- Reactor count vs worker count topology (R:W ratio; pinning policy).
- eBPF CID steering (opt-in optimization, later).
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
