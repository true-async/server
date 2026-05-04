# TODO — HTTP Server Performance Backlog

## Step 1 — HTTP/3 hot path

Most of this is already done: handler coroutines do not suspend on write, UDP_SEGMENT / GSO batching is implemented in `src/http3/http3_listener.c:584` (`http3_listener_send_gso`), and Linux uses direct `sendmsg(MSG_DONTWAIT)` bypassing libuv.

Remaining:
- `MSG_ZEROCOPY` for large QUIC DATA frames (> 8 KB) — can be addressed together with Step 3

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