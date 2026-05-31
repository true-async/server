# HTTP/3 roadmap — status

Living status of the staged HTTP/3 work from issue **#59** (closed: the
critical c=64 collapse was fixed; later perf phases were always deferrable
per the issue). Update this as each phase lands. Benchmarking method and
traps live in [`H3_BENCHMARKING.md`](H3_BENCHMARKING.md).

_Last updated: 2026-05-31._

## Staged phases (#59)

| Phase | What | Status | Where |
|-------|------|--------|-------|
| 0 | Baseline + measurement harness | ✅ done | `2221508` |
| 1 | Deferred / coalesced output (once-per-tick flush) | ✅ done | `68812fe` |
| 2 | **Pacing aligned to GSO burst** (`ngtcp2_conn_get_send_quantum`) | ✅ opt-in (`setHttp3Pacing`, default off) | #70 / `2eb2203` |
| 3 | **ACK-frequency / delayed-ACK tuning** | ❌ blocked (ngtcp2 1.22.1 lacks the extension) | — |
| 4 | UDP_GRO on receive (split coalesced datagrams) | ✅ done | `615ccd4` |

> **Validation constraint (this box).** Phases 2–3 are *real-path* optimizations
> whose benefit shows only under loss / rate-limiting. The test box is WSL on
> **loopback (lossless)** and its kernel has **no `sch_netem`**, so a constrained
> path cannot be simulated. The #59 exit gate requires "no RPS regression" —
> which these phases cannot satisfy on loopback by construction (see Phase 2
> result below). Phase 2 therefore shipped **opt-in, default off** — the setter
> wires the pacer without touching the default drain path; Phase 3 still needs a
> real network (or a netem-capable kernel) to land.

## Done beyond the staged phases

| Item | Status | Where |
|------|--------|-------|
| c=64 collapse — per-peer handshake budget + 1 ms reactor throttle | ✅ merged | #59 / PR #61, php-async `1162c4b` |
| Dirty-list UAF (unlink-in-free + arm_timer guard) | ✅ merged | #62 / PR #63 |
| Static delivery — `$res->sendFile()` over H3 | ✅ merged | #60 / PR #64 |
| Static delivery — `addStaticHandler` mount-routing over H3 | ✅ merged | #60 / PR #65 |
| Throttle fix installed into canonical `php-release` | ✅ done | out-of-tree release rebuild |
| Tunable CT-out TLS BIO ring (`setTlsBufferBytes`) | ✅ merged | #67 |
| Pacing aligned to GSO burst (`setHttp3Pacing`, opt-in) | ✅ merged | #70 |

## Landed — Phase 2 pacing (opt-in)

### Phase 2 — Pacing aligned to GSO burst `[MED / MED]` — ✅ shipped opt-in (`setHttp3Pacing`, default off)
By default `http3_connection_drain_out` (`src/http3/http3_io.c`) ships the
full burst (GSO batch up to 64 × 1500) without consulting the pacer.
`HttpServerConfig::setHttp3Pacing(true)` (default **off**) opts into pacing:
cap each drain at `ngtcp2_conn_get_send_quantum()`, call
`ngtcp2_conn_update_pkt_tx_time()` after the writev sequence, and yield to
the timer only when the inter-burst gap exceeds ~1 ms (so loopback still
drains inline). With pacing off the drain path is byte-for-byte the prior
behaviour — **zero** effect on the default path. Plumbed
config → shared → view → accessor (mirrors `isHttp3AltSvcEnabled`); the
drain reads `c->view->http3_pacing`. Test **029**.

**Why off by default:** on lossless loopback pacing **regresses 512 K
throughput ~29 %** (7 → 9 ms median) — it adds cost where there is no loss
to mitigate, and the benefit can't be shown without a constrained path (no
`sch_netem` here). Flip it on and re-measure on a real network: confirm
pacing reduces retransmits / improves goodput under loss. **Reference:**
quicly co-designs pacing + GSO (`deps/quicly/include/quicly/pacer.h`,
burst 8–10 MTU = one `sendmsg`); nginx is window-only.

## Remaining

### Phase 3 — ACK-frequency / delayed-ACK tuning `[LOW / LOW]` — ❌ blocked on ngtcp2
Relying on ngtcp2 defaults — no ACK tuning. **Blocked:** the bundled ngtcp2
1.22.1 does not implement the ACK-frequency extension — its `ngtcp2.h` has
**no** `ack_frequency` / `min_ack_delay`, only `max_ack_delay` (which tunes
*our* ACK delay, of marginal value for download-heavy H3). Advertising
`min_ack_delay` / ack-frequency transport params needs an ngtcp2 upgrade
first. **Reference:** h2o ACK-frequency + delayed ACK + packet-tolerance;
nginx `NGX_QUIC_MAX_ACK_GAP = 2`, immediate on reorder.

### Later phases — connection migration / multi-worker
- **CID steering** — without it, client migration or `SO_REUSEPORT` rehash
  routes a packet to the wrong worker → stateless reset → disconnect.
  References: nginx eBPF `SK_REUSEPORT`; h2o encodes `thread_id` in the CID
  and forwards.
- **H3-1 fabricated local sockaddr** (`http3_build_listener_local`) — the
  real local address is only needed for migration path-validation. **Not an
  upstream blocker:** the listener already owns a raw fd with `recvmsg` /
  `sendmsg` + cmsg (recvmmsg/GRO/ECN, errqueue, GSO send), so the real
  destination can be recovered via `IP_PKTINFO` here — no
  `zend_async_udp_sockname` required. Wire it up when migration lands.

### Memory
- **Per-conn inflation — not yet measured for H3.** An earlier draft cited
  ~0.7–1 MB/conn (≈23 GB worst-case); that figure was unfounded and is
  removed. The only measured number we have is **H2-TLS ~250–350 KB/conn**
  (worst ~11 GB) — a different protocol path (see the H2 memory notes), not
  H3. Measuring H3 needs a client that holds N concurrent connections, but
  `h3client` is single-shot, so H3 per-conn RSS is currently **unknown**.
  Next: build a multi-conn client, measure RSS, then start with
  `SSL_MODE_RELEASE_BUFFERS` and audit per-conn allocations.
  _Validatable on this box (RSS) once a multi-conn client lands; no network needed._

## Bugs found during roadmap work

- **Large response body over H3 "stall" was the test client, not the server
  (fixed #69).** A buffered (`setBody`) response above ~512 KiB–1 MiB appeared
  to hang (client got `200`, then 0 body bytes until timeout). Root cause was
  the single-shot `h3client` harness: its stream/connection flow-control
  window was 1 MiB and it didn't drain the socket per wakeup. With the window
  raised to 16 MiB + a per-wakeup socket drain + a larger `SO_RCVBUF` (#69),
  the **server delivers an 8 MiB body correctly** — the server side was fine.
  Regression-guarded by test **028**.

## Per-phase exit gate (from #59 — run after EVERY phase)

1. Code-quality review — no dead code / redundant NULL entry-guards; cleanup
   blocks blank-line-separated; blank line after closing `}`.
2. Comments ≤ 1–2 lines; drop TODOs the phase resolved.
3. C `const` style — `const` only before the type; on read-only value params
   and init-and-read locals.
4. Copy audit — no new copy on the per-packet path without justification.
5. Perf — `strace -c` syscall deltas (sendmsg/recvmsg per request) + 3×
   `h2load` median (release build, `php-release`) for 3 B / 16 K / 1 MB at
   c=10/100/1000, vs h2o & nginx. No RPS regression vs the pre-phase baseline.
6. Tests green — full `tests/phpt/server/h3/`, fuzz, and the phase's new tests.
