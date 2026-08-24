# Benchmarks

One entry per measurement, newest first. An entry names the machine, the build and
the scenario, because a number without them cannot be compared with the next one.

## 2026-08-24 — what #235 gives back by stating the count as an nv entry

Builds: `538f37e`, `main` before the change, against `8dc2913`, which is #235.
The measured tree differs from that commit in comment text alone.
Machine: WSL2, Linux 6.6.114.1, 16 cores. PHP 8.6.0-dev ZTS **release**
(`/home/edmond/php-release-26`), built for this measurement because the extension
now needs TrueAsync ABI 0.26.0 and the release binary the entries below used is
0.25.0. Server, route and load are the ones the entry below used, so the two
numbers can be compared: `tests/perf/servers/server_setbody.php` in `h2c` mode,
one worker, `/b3`, `h2load -n 100000 -c 16 -m 16` after a 20000-request warm-up
against the same process. The two builds alternate inside each round, and the
order flips every round, so neither the machine's drift nor the penalty of
running first falls on one build. 20 rounds, the first dropped for a cold page
cache.

| | req/s |
|---|---|
| before, median of 19 | 250865 |
| after, median of 19 | 257025 |
| paired ratio after/before, median | 1.0357 |
| paired ratio, min and max | 0.9860 … 1.2268 |
| rounds where #235 is faster | 15 of 19 |

**95 ns per response**, or 3.6% of a 3-byte one. Split by which build ran first
the ratio is 1.0357 over the nine rounds that started with `main` and 1.0268 over
the ten that started with #235, so the gain survives the order rather than riding
on it. The entry below measured the insert at 126 ns, and the two numbers are the
same effect from either side: what a `zend_hash_update` and two `zend_string`
allocations cost a buffered response.

The before build's own spread is 23% against the after build's 10%, both driven
by two rounds near 211000 that the paired ratio absorbs. HTTP/3 and the reactor
pool take the same change and are not measured.

## 2026-08-22 — what #200's Content-Length insert costs per HTTP/2 response

Builds: `b0ca84c`, the commit before #200, against `ebbe410`, which is #200.
Machine: WSL2, Linux 6.6.114.1, 16 cores. PHP 8.6.0-dev ZTS **release**
(`/home/edmond/php-release-24`), the same binary the #179 entry above used.
Server: `tests/perf/servers/server_setbody.php` in `h2c` mode, one worker, route
`/b3` — a 3-byte buffered body, so the response is nearly all framing. Load:
`h2load -n 100000 -c 16 -m 16` after a 20000-request warm-up against the same
process. The two builds alternate inside each round, so a drift in the machine
hits both; 18 rounds, the first of each of the two batches dropped because a
cold page cache cost the first process 15%.

#200 makes every buffered HTTP/2 and HTTP/3 response write its byte count into
the header table through `http_response_commit_content_length` — a
`zend_hash_update` plus two `zend_string` allocations, for a field that used to
be dropped and cost nothing.

| | req/s |
|---|---|
| before, median of 16 | 270693 |
| paired ratio after/before, median | 0.9671 |
| paired ratio, min and max | 0.9224 … 1.0030 |
| rounds where #200 is slower | 14 of 16 |

**126 ns per response**, or 3.3% of a 3-byte one. The single-build spread is 6%,
which is why the comparison is paired: taken as two independent medians the same
data reads −2.9%, and that is inside one build's own range.

The number is the whole per-response cost and does not scale with the body, so
its share falls as the body grows. `/b64k` was measured to show that and cannot:
seven rounds spread 27401 to 45520 req/s, 66% either way, and a 3% effect does
not survive it.

HTTP/3 is not measured. It calls the same function
(`src/http3/http3_callbacks.c:902`), so the 126 ns is spent there too, but what
share of an HTTP/3 response that is has not been taken.

The insert shows, so the shape the plan named next is worth trying: hand the
count to the flatten loop as an extra entry instead of putting it through the
table.

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

### The header block in the same frame

The first `write()` sent the status line and headers, then the frame. Carrying
the block inside the frame removes one write and one round trip from the byte a
client waits for. Alternating builds again, three rounds, median of three runs
each:

| response | frame only | headers in the frame | |
|---|---|---|---|
| one 4 KiB chunk | 22981, 23868, 23387 | 30057, 31601, 29732 | +28.5%, the shape an SSE response has |
| one 64 KiB chunk | 16738, 16565, 16709 | 16376, 16657, 16477 | unchanged: the block is small against the frame |

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

## Syscalls per streamed chunk on HTTP/1, plaintext (2026-08-20)

Taken to decide #179, which was argued from "three submits and up to three suspensions
per chunk" — a number nobody had measured. Release PHP at `/home/edmond/php-release-24`
(ABI 0.25), the extension built from `e246dcf`, `tests/perf/servers/server_stream.php` in
`h1` mode under `strace -f -c`. Two runs per chunk size differing only in how many chunks
each response carries, so the difference divides out the startup and the request itself.

| chunk | chunks measured | writev | epoll_pwait | per chunk |
|---|---|---|---|---|
| 4 KiB | 960 (1280 − 320) | 960 | 1009 | 1.00 writev, 1.05 waits |
| 64 KiB | 480 (640 − 160) | 480 | 510 | 1.00 writev, 1.06 waits |

One vectored submit and one park per chunk, flat in the chunk size — the 64 KiB frame
takes the copy-free path and still leaves as a single `writev`, because its three pieces
are three slots rather than three calls.

The same pair of runs over TLS, HTTP/1.1 on a TLS listener (`curl --http1.1`, bodies
verified at 262144 and 65536 bytes):

| chunk | chunks measured | write | epoll_pwait | per chunk |
|---|---|---|---|---|
| 4 KiB | 480 (640 − 160) | 249 | 69 | 0.52 writes, 0.14 waits |

Fewer, not more: `tls_push` copies the chunk into the plaintext BIO ring and the drain
writes what has accumulated, so two chunks share one socket write and most never park.

What it decides: the premise under #179 is gone. What a per-connection outbound queue
could still remove is the one park per chunk, and only by making the write
fire-and-forget — which is what `isWritable()` and `tryWrite()` would then be unable to
answer honestly.
