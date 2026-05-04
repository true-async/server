# TODO — HTTP Server Performance Backlog

## Step 1 — HTTP/3 hot path

**Goal**: fire-and-forget write for QUIC/UDP, matching what steps 1–4 did for HTTP/1 and HTTP/2.

- Add `ZEND_ASYNC_UDP_SENDTO_EX` or unify with existing `ZEND_ASYNC_UDP_REQ_F_*` flags into a single API
- Profile under `h3load` or `curl --http3-only` with keep-alive
- Consider UDP Generic Segmentation Offload (`UDP_SEGMENT`) for batching small QUIC packets
- Consider `MSG_ZEROCOPY` for large DATA frames in QUIC (> 8 KB)

## Step 2 — Full TLS optimization (deferred)

When revisited:
- `setsockopt(TCP_ULP, "tls")` — kernel TLS offload (kTLS)
- `SSL_sendfile` for large responses
- Reduce memcpy between BIO ring buffers
- Switch to socket-BIO to eliminate the extra copy layer

## Step 3 — Zero-copy for large responses

**Goal**: avoid CPU cost of copying large response bodies into kernel on send.

- Threshold-based: apply only when `len > 16 KB` (page-pin overhead makes it harmful for small responses)
- Add a flag to `ZEND_ASYNC_IO_WRITE_EX` to request zero-copy mode
- `libuv_reactor.c`: direct `send(MSG_ZEROCOPY)` bypassing libuv, drain error queue via `recvmsg(MSG_ERRQUEUE)` to invoke `free_cb`
- `iouring_reactor.c`: `IORING_OP_SEND_ZC`

**Expected effect**: 10–30% CPU saving on large-body responses; more significant on NUMA under L3 cache bandwidth pressure.