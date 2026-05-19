# HTTP/2 TLS emit strategies

The HTTP/2 emit pump that pushes nghttp2's outbound queue onto the wire
supports two strategies on the TLS path, plus a hybrid that picks between
them per pass. Issue
[#30](https://github.com/true-async/server/issues/30).

Plaintext HTTP/2 (h2c) is unaffected — it always uses zero-copy `writev`
through libuv. The strategies described below apply only to `conn->tls
!= NULL`.

## Why two strategies

A single TLS record holds at most ≈16 KiB of plaintext (`TLS_MAX_RECORD_PAYLOAD`).
OpenSSL's `SSL_write` takes one contiguous buffer — there is no
`SSL_writev` in the public API. That forces every emit pass to choose:

- pay one `memcpy` to coalesce frames into one buffer, then issue one
  `SSL_write_ex` (one cipher setup); or
- skip the gather, drive nghttp2 frame-by-frame into a memory BIO, and
  let `SSL_write` fire multiple times as the BIO ring fills.

The first wins when the coalesced plaintext is large enough to amortise
the per-pass bookkeeping cost. The second wins when the response is so
short that the per-pass bookkeeping dominates everything else.

## DRAIN

Sequence per emit pass:

```
http2_session_emit (TLS branch, DRAIN selected)
  └─ while (nghttp2_session_want_write(session))
       ├─ guarantee = BIO_ctrl_get_write_guarantee(tls_plaintext_bio)
       ├─ if (guarantee < 16 KiB + headroom) break
       ├─ n = http2_session_drain(session, buf[16 KiB])
       │     └─ nghttp2_session_mem_send  (memcpy frame_header + DATA payload
       │                                   into nghttp2's internal slice;
       │                                   data_source.read_callback runs
       │                                   in non-NO_COPY mode and copies
       │                                   response_body into the slice)
       │     └─ memcpy(slice → buf[16 KiB])
       ├─ BIO_write(tls_plaintext_bio, buf, n)   ← copy into BIO ring
       └─ loop
  └─ tls_drain(conn)                             ← SSL_write reads ring,
                                                   encrypts, ships cipher
                                                   bytes via libuv
```

Properties:

- nghttp2 is driven through `nghttp2_session_mem_send`, **not**
  `nghttp2_session_send`. The strategy-side `h2_send_callback` and
  `h2_send_data_callback` are never invoked.
- `session->emit_state` stays `NULL`, so `http2_response_data_read`
  takes the non-NO_COPY branch and copies the body into nghttp2's
  buffer.
- Zero per-pass `emalloc`. The 16 KiB drain buffer lives on the stack
  for the duration of `http2_session_emit`.

## GATHER

Sequence per emit pass:

```
http2_session_emit (TLS branch, GATHER selected)
  ├─ session->emit_state = &st;
  └─ nghttp2_session_send(session->ng)
       ├─ h2_send_callback        — memcpy frame bytes  → emit_buf,
       │                            append (offset,len) → records[]
       ├─ h2_send_data_callback   — for DATA frames the data_source ran
       │                            in NO_COPY mode, so we only memcpy
       │                            the 9-byte frame header into emit_buf
       │                            and append (body_ptr, len) → records[];
       │                            GC_ADDREF the body zend_string and
       │                            stash it in body_refs[]
       └─ … repeats until byte_cap or queue drained
  └─ h2_emit_flush_tls_records(st)
       ├─ walk records[]: memcpy each slice (emit_buf bytes or body bytes)
       │                  into stage[16 KiB]
       └─ SSL_write_ex(stage, stage_len)
  └─ release body_refs[]; efree(records[]); efree(body_refs[]); reset st
```

Properties:

- One large `SSL_write_ex` per pass — one cipher setup amortised over
  the whole `stage[]` payload.
- Per-pass overhead: `emalloc(records[])`, `emalloc(body_refs[])`,
  N × `GC_ADDREF`, later N × `OBJ_RELEASE`, then two `efree`s.
- Body memory is referenced through `body_refs[]` (zval refcount), not
  copied into `records[]` itself — only the gather into `stage[]` copies
  body bytes.

## HYBRID

Selector lives in `http2_session_emit` and is one boolean:

```c
const bool use_drain =
    (mode == H2_EMIT_DRAIN) ||
    (mode == H2_EMIT_HYBRID && session->large_streams_pending == 0);
```

`large_streams_pending` is an `unsigned` counter on `http2_session_t`,
pinned by streams that exceed `H2_TLS_HYBRID_LARGE_THRESHOLD` at submit
time (or whose final size is unknown, e.g. streaming responses):

| Submit site | Pins counter when |
|---|---|
| `http2_session_submit_response` | `body_len > THRESHOLD` |
| `http2_session_submit_response_streaming` | always (size unknown a priori) |
| static handler (HEAD with inline body) | `stream->response_body_len > THRESHOLD` |
| static handler (file streaming) | `body_length > THRESHOLD` |

The counter is released in `cb_on_stream_close`. Idempotent via the
per-stream `counted_large` flag — one pin/unpin per stream regardless
of how many submit sites the stream passed through.

Behaviour by workload:

| Workload | counter | path used |
|---|---|---|
| JSON API, every response < threshold | always 0 | DRAIN always |
| Single large download in flight | 1 | GATHER until close |
| Streaming response (unknown size) | 1 | GATHER until close |
| Mixed: short requests during a large stream | ≥ 1 | GATHER (including the short ones) |

The last row is deliberate conservatism. A single pass may serialise
frames from several multiplexed streams, so we cannot pick a
per-stream path inside one nghttp2 send cycle. While any large stream
is active, the whole pass uses GATHER; this is a strict superset of
the work needed and avoids any case where DRAIN would force an
under-amortised series of small `SSL_write` calls.

### Override

`TRUE_ASYNC_H2_TLS_EMIT_MODE` env var, read once at process start:

| Value | Effect |
|---|---|
| `hybrid` (default) | per-pass auto-select as above |
| `drain` | force DRAIN |
| `gather` | force GATHER |

## The arithmetic

Per single response (HEADERS ≈ H bytes, body B bytes) on a 1-RTT pass.

### Body byte movement

```
DRAIN:    response_body ──memcpy──► nghttp2 slice
                        ──memcpy──► buf[16 KiB]
                        ──memcpy──► BIO ring plaintext
                        ─encrypt──► BIO ring ciphertext
          ───────────────────────────────────────
          3 user-space copies of B bytes + 1 encryption

GATHER:   response_body ──pointer + GC_ADDREF──► records[]
                        ──memcpy──► stage[16 KiB]
                        ─encrypt──► OpenSSL out-record
          ───────────────────────────────────────
          1 user-space copy of B bytes + 1 encryption

kTLS (Phase 2):
          response_body ──pointer──► iov[i].base
                        ──sendmsg(fd, iov, niov)──► kernel scatter-gather
                        ─encrypt──► kernel TLS record (in-kernel memcpy
                                    fused with AES round)
          ───────────────────────────────────────
          0 user-space copies, 1 kernel-fused copy+encrypt
```

### Per-pass overhead

| Cost | DRAIN | GATHER | kTLS |
|---|---|---|---|
| `emalloc` calls | 0 | ~2 (records + body_refs) | 0 |
| `efree` calls | 0 | ~2 | 0 |
| `GC_ADDREF` / `OBJ_RELEASE` pairs | 0 | N (one per body slice) | 0 |
| Stack buffers | one 16 KiB | one 16 KiB stage | one iov[] |
| `SSL_write*` calls | 1-N (one per BIO fill) | 1 (one per stage flush) | 0 (kernel does it) |

### Where the cross-over sits

Two competing costs:

1. **Body memcpy bandwidth**. DRAIN copies B bytes three times in
   user-space; GATHER copies them once. For body B in DDR cache that's
   `2 × B` extra bytes for DRAIN.
2. **Per-pass bookkeeping**. GATHER pays two `emalloc/efree` round-trips
   plus N `GC_ADDREF`/`OBJ_RELEASE` pairs — roughly 100-300 ns of fixed
   overhead per pass on modern hardware.

DRAIN wins while the bookkeeping cost outweighs `2 × B` of memcpy.
GATHER wins once `2 × B` is bigger than the bookkeeping. The crossover
sits well below one TLS record because each `SSL_write` on the cipher
side carries its own ~250-500 ns of cipher state setup — and DRAIN
typically triggers multiple `SSL_write` cycles per response (HEADERS
frame and DATA frame fill the BIO ring in separate `BIO_write` calls
once the body is non-trivial).

Measured on h2load, 1 worker, c=100 m=32, release PHP, 10 s ×3 median:

| body | GATHER RPS | DRAIN RPS | winner |
|---|---:|---:|---|
| 3 B (dynamic) | 137 911 | 160 875 | DRAIN +17 % |
| 100 B (static) | 92 829 | 121 429 | DRAIN +31 % |
| 4 KiB (static) | 57 211 | 49 805 | GATHER +14 % |
| 16 KiB | 32 954 | 22 723 | GATHER +45 % |
| 64 KiB | 10 469 | 6 518 | GATHER +60 % |

The threshold lives in `include/http2/http2_session.h` as
`H2_TLS_HYBRID_LARGE_THRESHOLD`. It is tuned to track this crossover —
see the bench numbers above when changing it.

## Phase 2 outlook (kTLS)

When `setsockopt(TCP_ULP, "tls")` is available and the negotiated cipher
maps to a kernel TLS crypto info struct, encryption moves into the
kernel. `SSL_write` is replaced by ordinary `sendmsg(fd, iov, niov)` —
kernel TLS reads the scattered iov elements into one record, encrypts
inline with AES round, and ships the cipher bytes.

The HTTP/2 emit pump on the kTLS path becomes the existing h2c
`records[]`/`iov[]` machinery, unchanged. The memory BIO pair and
`tls_drain` go away. GATHER remains as the fallback for platforms or
kernel versions without `tls.ko` (Windows, BSD-RX, pre-4.13 Linux).

DRAIN does not survive Phase 2 — once the user-space gather memcpy is
gone (kTLS does it in-kernel for free), there is no remaining cost for
GATHER's bookkeeping to amortise against on short responses, and the
iov-based emit beats DRAIN on every body size.

See [issue #31](https://github.com/true-async/server/issues/31) when
opened for the Phase 2 plan.
