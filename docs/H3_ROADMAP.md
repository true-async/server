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
| 2 | **Pacing aligned to GSO burst** (`ngtcp2_conn_get_send_quantum`) | 🚧 blocked on validation | — |
| 3 | **ACK-frequency / delayed-ACK tuning** | 🚧 same blocker expected | — |
| 4 | UDP_GRO on receive (split coalesced datagrams) | ✅ done | `615ccd4` |

> **Validation constraint (this box).** Phases 2–3 are *real-path* optimizations
> whose benefit shows only under loss / rate-limiting. The test box is WSL on
> **loopback (lossless)** and its kernel has **no `sch_netem`**, so a constrained
> path cannot be simulated. The #59 exit gate requires "no RPS regression" —
> which these phases cannot satisfy on loopback by construction (see Phase 2
> result below). They need a real network (or a netem-capable kernel) to land.

## Done beyond the staged phases

| Item | Status | Where |
|------|--------|-------|
| c=64 collapse — per-peer handshake budget + 1 ms reactor throttle | ✅ merged | #59 / PR #61, php-async `1162c4b` |
| Dirty-list UAF (unlink-in-free + arm_timer guard) | ✅ merged | #62 / PR #63 |
| Static delivery — `$res->sendFile()` over H3 | ✅ merged | #60 / PR #64 |
| Static delivery — `addStaticHandler` mount-routing over H3 | ✅ merged | #60 / PR #65 |
| Throttle fix installed into canonical `php-release` | ✅ done | out-of-tree release rebuild |
| Tunable CT-out TLS BIO ring (`setTlsBufferBytes`) | 🔄 PR | #67 |

## Remaining

### Phase 2 — Pacing aligned to GSO burst `[MED / MED]` — 🚧 implemented, not merged
`http3_connection_drain_out` (`src/http3/http3_io.c:194`) ships the full
burst (GSO batch up to 64 × 1500) without consulting the pacer. **Approach
(prototyped, then reverted):** cap each drain at
`ngtcp2_conn_get_send_quantum()`, call `ngtcp2_conn_update_pkt_tx_time()`
after the writev sequence, yield to the timer only when the inter-burst gap
exceeds a threshold (so loopback drains inline), and arm the timer from
`drain_out` itself so the remainder reschedules regardless of caller.

**Result:** correct — no stall (h3 suite 27/27, 64 K–512 K bodies complete).
But on lossless loopback it **regresses 512 K throughput ~29 %** (7 → 9 ms
median) because pacing adds cost where there is no loss to mitigate, and the
benefit can't be shown without a constrained path (no `sch_netem` here). So
it is **not merged**. Revisit on a real network: confirm pacing reduces
retransmits / improves goodput under loss, then land with that evidence.
**Reference:** quicly co-designs pacing + GSO (`deps/quicly/include/quicly/pacer.h`,
burst 8–10 MTU = one `sendmsg`); nginx is window-only.

### Phase 3 — ACK-frequency / delayed-ACK tuning `[LOW / LOW]`
Relying on ngtcp2 defaults — no ACK tuning. **Fix:** advertise
`min_ack_delay` / ack-frequency transport params (subject to ngtcp2
version support; document the floor). **Reference:** h2o ACK-frequency +
delayed ACK + packet-tolerance; nginx `NGX_QUIC_MAX_ACK_GAP = 2`,
immediate on reorder.

### Later phases — connection migration / multi-worker
- **CID steering** — without it, client migration or `SO_REUSEPORT` rehash
  routes a packet to the wrong worker → stateless reset → disconnect.
  References: nginx eBPF `SK_REUSEPORT`; h2o encodes `thread_id` in the CID
  and forwards.
- **H3-1 fabricated local sockaddr** (`http3_build_listener_local`, no
  `zend_async_udp_sockname`) breaks migration path-validation — upstream
  blocker for the migration phase.

### Memory
- **Per-conn inflation** ~0.7–1 MB/conn (≈23 GB worst-case on the arena).
  Start with `SSL_MODE_RELEASE_BUFFERS`, then audit per-conn allocations.
  _Validatable on this box (RSS), no network needed._

## Bugs found during roadmap work

- **Large response body over H3 stalls above ~512 KiB–1 MiB.** A buffered
  (`setBody`) response of 128 K / 256 K / 512 K completes; **1 MiB / 2 MiB /
  4 MiB stall** (client receives `200` then hangs, 0 body bytes, until
  timeout) — on clean `main`, independent of pacing. Reproducible on
  loopback. Likely the H3 stream flow-control window not extending for a
  large buffered body (or the buffered `data_reader` parking). **This is
  validatable here and worth fixing** — arguably higher value than the
  loss-path perf phases, which this box can't validate at all.

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
