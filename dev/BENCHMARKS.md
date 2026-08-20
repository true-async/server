# Benchmarks

One entry per measurement, newest first. An entry names the machine, the build and
the scenario, because a number without them cannot be compared with the next one.

## 2026-08-20 — what the three writes per HTTP/1 chunk cost (#179)

Branch `180-body-rename` at 2e72f41. Machine: WSL2, Linux 6.6.114.1, 16 cores.
PHP 8.6.0-dev ZTS **release** (`--disable-debug`), built for this measurement into
`/home/edmond/php-release-24` from the same php-src the debug build uses, because
the installed release PHP is ABI v0.23 and the extension needs v0.24. Load:
`wrk -t1 -c4 -d6s`, three runs per cell, median reported. Server:
`tests/perf/servers/server_stream.php` in `h1` mode, one worker.

`h1_stream_append_chunk` sends a chunk as three awaited writes — size line, body,
CRLF (`src/http1/http1_stream.c:154`). Counted with `strace -e trace=write,epoll_pwait`
on one request of four 16 KiB chunks: **3 `write(2)` and 3 zero-timeout
`epoll_pwait` per chunk**, whatever the chunk size, plus one write for the headers
and one for the terminator. The loop turn after each write is the coroutine
suspending: `async_io_req_await` returns early only on `req->completed`, and
libuv's inline-write fast path does not fire for back-to-back writes on one stream.

The comparison holds the body at 64 KiB and moves only the chunk count. The second
build differs by one hunk: the three pieces are copied into one buffer and sent as
a single awaited write.

| chunks | chunk | three writes | one write | gain | µs per chunk, before → after |
|---|---|---|---|---|---|
| 1 | 64 KiB | 15746 | 17207 | +9.3% | — |
| 4 | 16 KiB | 8320 | 11876 | +42.7% | 18.9 → 8.7 |
| 16 | 4 KiB | 2938 | 5209 | +77.3% | 18.5 → 8.9 |
| 64 | 1 KiB | 893 | 1650 | +84.8% | 16.8 → 8.7 |

Taken. Per-chunk cost is flat in the chunk size and halves when the frame goes out
as one write: the two extra syscalls and two extra loop turns are worth about
10 µs per chunk. The 1 KiB row is the noisiest — its three base runs were 1135,
893 and 781 — and the others repeat within 3%.

The prototype copies the whole chunk to coalesce it, and still wins by that much.
A vectored write would avoid the copy, but the ABI has no awaitable one:
`io_pipe_writev_cb` (`php-src/ext/async/libuv_reactor.c:4947`) sends no NOTIFY and
frees the request itself, so `ZEND_ASYNC_IO_WRITEV` cannot be awaited. Removing the
copy means adding that op to ext/async.

What this decides for #179: the win is reachable without a queue, without an
ordering hazard between two writers and without a per-response structure — the
three things both rejected designs foundered on. Whatever else #179 wants, a
non-blocking `tryWrite()` on HTTP/1, has to be argued on its own; this measurement
does not support it.

### Where the copy stops paying

Coalescing costs one user-space copy of the chunk, so it pays only while that copy
is cheaper than the two syscalls it removes. Body held at 1 MiB, chunk size moved,
five runs per cell, median:

| chunk | three writes | one write | gain |
|---|---|---|---|
| 1 KiB | 49 | 132 | +167% |
| 4 KiB | 226 | 374 | +65% |
| 16 KiB | 857 | 1319 | +54% |
| 32 KiB | 1486 | 1860 | +25% |
| 64 KiB | 2606 | 2197 | −16% |
| 128 KiB | 3616 | 3206 | −11% |
| 256 KiB | 4457 | 3546 | −20% |

The crossing is between 32 and 64 KiB, so `H1_CHUNK_COALESCE_MAX` is 32 KiB and a
larger chunk keeps the three-write path.

### The shipped change, verified against the noise

Runs of the same build drift by up to 9% on this machine, which is wider than some
of the gains above, so the shipped change was re-measured by alternating the two
builds — start, three `wrk` runs, stop, swap — three rounds each.

| chunk | three writes | shipped | |
|---|---|---|---|
| 64 KiB | 2362, 2409, 2511 | 2473, 2573, 2577 | same code path either side of the threshold; the spread is the noise floor |
| 4 KiB | 197, 243, 208 | 367, 376, 377 | +81%, and every run of one build is outside the other's range |

## 2026-08-19 — cost of the per-chunk flush on a streamed response (#170)

Base commit 22a8d37 plus the #170 working tree. Machine: WSL2, Linux 6.6.114.1,
PHP 8.6.0-dev ZTS DEBUG. Both runs on the same machine and the same PHP; the two
extension builds differ only in the flush. Byte counts do not depend on the build
mode, so DEBUG is not a caveat here.

Scenario: one streamed response of 4080 bytes (80 repeats of a 51-byte log line),
delivered as 1, 8 or 80 `send()` calls. The number is the encoded body on the wire,
de-chunked. Identity is 4080 bytes in every case.

| codec | chunks | before | after | per flush |
|---|---|---|---|---|
| gzip | 1 | 97 | 104 | 7.0 |
| gzip | 8 | 97 | 158 | 7.6 |
| gzip | 80 | 97 | 709 | 7.7 |
| br | 1 | 57 | 58 | 1.0 |
| br | 8 | 57 | 133 | 9.5 |
| br | 80 | 57 | 845 | 9.9 |
| zstd | 1 | 69 | 72 | 3.0 |
| zstd | 8 | 69 | 149 | 10.0 |
| zstd | 80 | 69 | 853 | 9.8 |

Taken. The worst case measured — 80 chunks of 51 bytes, the row-by-row export the
issue is about — costs 8 to 10 bytes per chunk and still sends 4.8 times less than
identity. A handler that wants the old ratio buffers its response instead of
streaming it, which is the same choice it always had.

Script: `tests/perf` has no case for this; the measurement was driven by an ad-hoc
client that reads the chunked body and reports its length, run once per extension
build. Reproduce by streaming a compressible body in N `send()` calls and comparing
the de-chunked body length across builds.
