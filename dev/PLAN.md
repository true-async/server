# Plan

Open steps, in order. A step is closed here when its evidence is in the repository:
a test that fails without it, a measurement, or both. Issue numbers point at
`true-async/server` unless another repository is named.

## In progress

- [x] **#170 — a streamed response is flushed per chunk.** `flush` op on the encoder
  vtable (`Z_SYNC_FLUSH`, `ZSTD_e_flush`, `BROTLI_OPERATION_FLUSH`), called from
  `ws_append_chunk` for every non-empty chunk. Unconditional on the streaming path,
  no config key. Evidence: `013-h1-streaming-gzip-flush.phpt` decodes the first chunk
  while the handler is still parked and 0 bytes without the fix; `042` and `051` show
  Brotli and zstd going from nothing on the wire to a flushed block; `CompressionGzip`
  and the new `CompressionFlush` unit tests decode the flushed prefix with each
  codec's own decoder, including through the `NEED_OUTPUT` loop; cost measured in
  `dev/BENCHMARKS.md`. Merged.

## S1 — A paused listener leaves new connections in the backlog (item 12)  [done]

Goal: `http_server_pause_listeners` stops accepting, so a connection that arrives
during a pause is served after the resume instead of answered 503.
Done when: `h1/062` passes on Linux, macOS and Windows CI against php-async `main`.
Tests: first
Base: 068cd09 (server), e580c15 (php-async)

Cause: php-async's `libuv_listen_stop` cannot stop `uv_listen` (libuv has no such
call), so accepts go on. Design E, by the Critic and the Sage on 2026-09-30,
approved by Edmond: `stop` makes the connection callback return without
`uv_accept`, and libuv stops watching the fd itself (`unix/stream.c:528-531`);
the first `start` runs `uv_listen`, a later one unpauses and delivers the held
connection on the next tick; Windows TCP listeners take one AcceptEx at a time,
set before `uv_listen`. Rejected: a `uv_poll` accept loop (a busy loop on a
socket several workers share on Windows, an EMFILE spin, no named pipes).

- [x] S1.1 php-async: an asynctest cmocka test of stop and start, red first
      done: stop, three connects, no callback; start, three callbacks on the next
        tick and none inside start(); pause and resume twice with a held
        connection; dispose while paused leaks nothing under ASan
      tier: T2 · role: —
      handoff: `sapi/asynctest/tests/test_listen_pause.c` in php-src (PR
        true-async/php-src#38), 5 cases, all red on php-async e580c15. Under
        ASan with LeakSanitizer (USE_ZEND_ALLOC=0): 5 of 5, 3 of 3 runs, no
        report; a leak planted in listen_close_cb is reported (528 bytes).
- [x] S1.2 php-async: design E in the listen event
      done: S1.1 green; the php-async phpt suite passes; a callback that returns
        without accepting while not paused is re-delivered, not lost
      tier: T2 · role: Critic
      Critic 2026-09-30 on the diff: single accept at creation slowed every
        Windows listener (libuv can switch after listen) — moved to the first
        stop(); the init-failure retry could spin — now a loud error; recover
        skips a closing handle; a failed accept waits for the next tick. All
        taken.
      handoff: php-async PR #306 (commit 7965109). asynctest 5 of 5 green, 3 of
        3 runs; php-async phpt 1214 of 1220, the 6 fail on main alike (a
        symlinked ext/async breaks their ../../../../ includes).
      Windows CI 2026-09-30: `h1/064` failed on #306. libuv keeps completed
        AcceptEx requests in a LIFO stack, and the switch to single accept at
        the first stop() completes only after all 32 pre-posted requests are
        used, so a pause delivered the newest connection first. Sage: single
        accept before the first uv_listen of every Windows TCP listener
        (php-async PR #307); a FIFO queue in php-async was rejected. 064 green
        on Windows with #307. Its accept-rate cost is item 33.
- [x] S1.3 server: 062, a burst test and a re-pause test green on the patched
        php-async; the comments at `http_server_class.c` on REUSEPORT and pause
        say what happens; the 503 safety net and dropped accept errors are counted
      done: the three phpt pass locally; `h1/062` fails on unpatched php-async
      tier: T1 · role: —
      handoff: 062, 063, 064 read 503 on php-async main and pass 10 of 10 on the
        branch; the whole suite 516 of 516 on it. telemetry/009 lists the two
        new counters. Issue #359.
- [x] S1.4 Both PRs: php-async first, then the server against its `main`
      done: both merged with every CI platform green; the Windows accept rate
        before and after single accept is recorded in `dev/BENCHMARKS.md`, or
        named as not measured
      tier: T1 · role: —
      handoff: php-src #38 (into true-async and true-async-stable), php-async
        #306 and #307, server #360 (bd919ae); every platform green. Windows
        accept rate not measured: item 33. Hand mutants (28.2) on the php-async
        diff: 6 of 10 killed by the asynctest. Survivors: dup() `< 0` to `<= 0`
        is equivalent (fd 0 stays stdin); the re-arm after a failed uv_accept
        cannot be reached on Linux; the two branches of the delivery loop need
        several held connections, which only a Windows AF_UNIX listener
        (pending pipe instances) produces: item 34.

## S3 — Windows problems of the server  [in progress]

Goal: the Windows build is tested on the same behaviour as Linux, except for
features Windows lacks, and its known flakes and unmeasured costs are settled.
Done when: every Windows CI skip has a reason or an open plan item, and S3.1,
S3.2 and S3.4 are closed with evidence.
Base: 05c871c (server), 1521d69 (php-async)
Notes: dev/plans/S3.md

- [x] S3.1 `multipart/009` fails on Windows (items 37 and 18)
      done: a local command reproduces the NULL `getSize()` at a stated rate;
        the fix brings it to 0 of the same number of runs; a test is red
        without the fix
      tier: T1 · role: —
      handoff: #368, PR 369; `multipart/025`, 0 of 1200 runs fail after (6-10 of
        400 before). The unlink after a failed `fopen` has no test: item 18.
- [x] S3.2 Decide how synchronous TransmitFile moves the file pointer
      done: a C probe answers three questions on this machine (does the
        pointer advance; what a count past the file end does; why `h1/065`
        got 0 bytes before php-async 1521d69); answers and the verdict on the
        clamp in `io_transmitfile_work_cb` recorded in `dev/DECISIONS.md`
      tier: T1 · role: —
      handoff: `dev/DECISIONS.md` 2026-09-30; comments in true-async/php-async#309.
- [x] S3.3 Sort the Windows CI skips (236 of 548, run 36767391692)
      done: each skipping phpt listed with its reason in `dev/plans/S3.md`,
        marked justified or given an open plan item; items 27 and 34 included
      tier: T1 · role: Critic
      handoff: `dev/plans/S3.md`: 226 skips hide code, 10 are justified.
- [x] S3.4 Connect storm on Windows before and after php-async #307 (item 33)
      done: time until N parallel connects are accepted, for both builds, in
        `dev/BENCHMARKS.md` with N, machine and run count
      tier: T1 · role: —
      handoff: `dev/BENCHMARKS.md` 2026-09-30: 1000 connects take 578-595 ms with
        #307 and 156-172 ms without, 7 runs each, A/B/A. The cost goes to S3.10.
        Scripts `storm*.php` are not in a repository.
- [x] S3.5 (#374) Drop the false "SO_REUSEPORT" skip from its 56 tests, and make the
        `curl --version 2>/dev/null` check of 22 test files run under cmd
      done: none of the 56 skips on Windows CI for either reason; each failure
        among them becomes a step of its own
      tier: T1 · role: —
      handoff: PR 377; Windows CI run 36828469004 skips none of the 56 for either
        reason (212 of 549 skipped, 0 failed). The 9 first failures were the
        tests' own POSIX shell; the pool imbalance `012` found is S3.11.
- [ ] S3.6 The Windows job builds ext/sockets, ext/openssl, zstd and both test
        hook options (added to `config.w32`)
      done: no test of those groups in `dev/plans/S3.md` skips on Windows CI for
        its listed reason; failures as in S3.5
      tier: T1 · role: —
- [ ] S3.7 A curl with HTTP/2, h2load and h2spec on the Windows runner
      done: no test of those groups skips on Windows CI for its listed reason;
        failures as in S3.5
      tier: T1 · role: —
- [ ] S3.8 HTTP/3 in the Windows job (`--enable-http3`, ngtcp2, nghttp3,
        OpenSSL >= 3.5 on the runner)
      done: no HTTP/3 test skips on Windows CI for HTTP/3 missing; failures as
        in S3.5
      tier: T2 · role: Critic
- [ ] S3.9 Port the six tests whose POSIX mechanism is only their setup
        (`dev/plans/S3.md`)
      done: the six run and pass on Windows CI
      tier: T1 · role: —
- [ ] S3.10 Decide whether php-async keeps one AcceptEx at a time on Windows,
        at 3.5 times the time for a 1000-connect burst (S3.4)
      done: choice and reason recorded in `dev/DECISIONS.md`
      tier: T2 · role: Critic
- [ ] S3.11 (#375) A Windows worker pool serves from one worker: worker 0 accepted
        nothing in 30 of 30 runs, the worker started last took every request in
        23 (`dev/plans/S3.md`, S3.5)
      done: the cause found (shared socket, one AcceptEx at a time, or the
        reactor) with the failing run named; the pool spreads or the limit is
        decided in `dev/DECISIONS.md`; USAGE.md:688 states the measured behaviour
      tier: T2 · role: Critic

## Next

### Order of the open defects, 2026-09-28

Defects before features (#133, #106, #72, #48, #6 wait). Fixed in this order;
an item marked "reproduce" gets its failing run before any code.

1. #311 — lingering close over TLS. Done.
2. #312 — permessage-deflate close: reproduced and fixed with #311. #327, a
   frame queued while a send is parked in its write, found through `035` on
   Windows CI and fixed in the same PR.
3. #313 — done: the ratchet (PR 331), `core/023` and `tls/003` fixed with an
   empty baseline (PR 333, which also fixed #332), the first attempt's diff
   printed before a retry (PR 334). The clock removal is dropped (Sage).
4. #336 — SSE byte count: done (PR 337).
5. #322 — upload limits as PHP: `max_file_uploads` (extra files left out),
   `max_multipart_body_parts`, `file_uploads`, `MAX_FILE_SIZE`; no per-file cap
   past `setMaxBodySize()`. `upload_max_filesize` is not read, by Edmond on
   2026-09-30: its 2M default would cut uploads the body limit admits. The
   design went through the Critic. A `..` in a client filename answering 101
   is split out as #339.
6. #341 — full read buffer on plaintext HTTP/1: done. The read pauses by
   disposing the multishot request (the reactor's own `uv_read_stop`) and
   resumes at half a buffer; no php-async change. Designed by the Architect,
   reviewed by the Critic (two remarks taken, no dispute left for the Sage).
   Its `h1/061` found #345, fixed first (PR 347): `stop()` left connections
   open, and they started requests in the server scope `start()` had
   cancelled. With both, `h1/061` passes 30 of 30 runs; 3 to 4 of 30 crashed
   before.
7. #315 — iterative matcher in the Windows fnmatch shim: done. The matcher is
   `http_static_glob_match`, built on every platform; `StaticHide` checks it
   against POSIX `fnmatch` over 19 patterns x 18 paths both ways, and a
   4 KiB path the recursive form did not finish in 30 s passes under 1 s.
8. #350 — worker path: compression and protocol version, done. A pooled response
   is encoded as in-thread HTTP/3 encodes it, a `Content-Encoding` body is
   decoded, `json()` follows the flags, and the sendFile wire carries
   Accept-Encoding so a `.gz` sidecar is served. The dropped FULL wire moved to
   #351: the Critic found the peer is left waiting and the other wire kinds and
   the TTL drops go uncounted too, which needs its own design (a reactor-side
   reset, a label that is not `wire_failed`). Pooled requests also lose their
   trace context: #352.
9. #354 — `test_static_decoders` back in the unit suite: done. The CMake target
   has no existence guard now, the cases call `http_mime_lookup_by_ext` and
   `http_conditional_check` (moved from `send_file.c` into `http_etag.c` so the
   test links without the engine), and a boundary case was added after `<=`
   turned into `<` passed all 32: 33 cases, `ctest` 22 of 22.
10. #314 — `compression/070` hung 120 s on the Windows job twice on 2026-09-30
    (PR 340, then PR 349's rebase), passing on retry each time. Not reproduced
    on Linux. It is the only test that pipes through a `gzip -d` child 30 times
    per run; 010, 012 and 030 do it once and never hung. The four now encode and
    decode with ext/zlib in-process (static in the Linux and Windows builds), so
    the suspect `proc_open()` pipe path is out of the suite. Closed if 070 does
    not hang again; the pipe hang itself, if real, belongs to php-async.
11. HTTP/3 `chunk_queue` primer: with the next change to `h3_stream_append_chunk`.
12. `http_server_pause_listeners` and the accept it may not stop: stage S1
    above (#359, true-async/php-async#305). The accept errors it used to drop
    are counted in `accept_errors_total`; they are still not logged.
12a. A failed or short HTTP/1 file body kept the connection alive: done,
    stage S2 (#362, true-async/php-async#308); tests `h1/065` to `h1/068`.
12b. The same on HTTP/2: done (#364). A failed or short file body is marked
    `local_aborted`, and the data provider answers the drained ring with
    `NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE`, which nghttp2 turns into
    RST_STREAM(INTERNAL_ERROR); the unread part of a short chunk is debited from
    the static budget, which leaked it before. Tests `h2/071` (truncation),
    `h2/072` (fault point `h2/file_body/io_error`). The provider's copy branch
    (`h2_dp_streaming_copy`, a flush outside emit) has no test.
13. `core/018` under load: done. The log ring's drop is designed and counted,
    so the test moves, not the sink: `--EXPECTREGEX--` admits the sink's
    `ring overflow, dropped=N` notice before the output and nowhere else. The
    notice appeared in 2 of 12 direct runs, six in parallel; the test passes
    18 of 18 under `-j6`, none on retry.
14. The port helpers race: done, not reproduced. `tas_free_port` handed out a
    kernel-assigned ephemeral port and closed it, so an outgoing connection
    could take it before the server bound it (`core/027`, macOS, run
    36718896142; `core/023`, Windows, PR 337). The helpers now take ports from
    21000-27999, below every platform's ephemeral range and clear of the
    literal ports older tests use, and hold a flock()ed file per port for the
    life of the test process; TCP and UDP are probed. On Linux the window did
    not fail once in 3000 tries, with a 500-port range and two processes
    churning connections, old helper or new, so the evidence is the mechanism
    and the macOS and Windows CI legs over the coming runs. Port 0 listeners
    (81c0437) stay the stronger form for tests that can read
    `getBoundListeners()`.
15. #346 — the reactor-pool worker inbox after `stop()`: done. A handler's
    `stop()` on its worker clone left the inbox in the registry; on main
    `h3/079` reads 6 to 10 of 16 requests after the stop answered by the
    stopped clone, `h3/080` 6 to 7 of 16 sent while the stopping handler held
    its thread, 3 runs of 3 each, and both pass with the fix. `do_stop`
    unpublishes the inbox; `start()` fences the reactors after its wait and
    waits out a retire the pool coroutine still runs; the bailout and free
    paths unpublish before the free. Mutants: dropping the `do_stop` unpublish
    fails `h3/080`; dropping the fence in `start()` passes every local pool
    test (a dispatch that loaded the inbox just before the unpublish, no hook
    widens it). The restart half is unreachable: the pool loads a fresh clone
    per worker run and starts it once.
15a. #376 — `start()` from a handler after `stop()`: done, refused. Waiting
    for the drain is not an option: the drain waits for that handler. A
    `start_active` flag spans `start()` from entry to return; a `start()` while
    it holds on a stopped server throws "Server is still stopping" before it
    touches the server. `core/084` reads a 500 for the handler's own request
    and the misleading room hub error against main, 3 runs of 3, and passes
    here; it also kills the mutant whose refused `start()` on a running server
    clears the flag of the run on the stack. The flag's reset on the bailout
    exit has no test.
16. #348 — static passthrough on HTTP/1: done, PR 349 (09ab593). A file under
    `on_missing: Next` that fails the engine's `open()` (mode 0) started the
    handler twice on one ctx and crashed, 3 of 3; `static/025`.

Added by the health check of 2026-09-30 (`dev/HEALTH.md`), in this order:

17. The HTTP server test hooks run in CI: the debug leg passes
    `--enable-http-server-test-hooks`, and the 20 phpt gated on `_http_*`
    functions (reactor_pool, telemetry 001-003, eight in core) run and pass.
    Locally, built with the flag: `core/030` crashed 3 of 3 in php-async's
    `io_pipe_write_cb`, which cleared `io` on a write request its listener had
    freed and reallocated (true-async/php-async#310, fix #311, asynctest case
    true-async/php-src#39). With the fix the 20, with the four tas-hooks tests
    and the rest of `reactor_pool/`, pass 35 of 35, 3 runs of 3. In CI the
    first run of this change crashed `core/030` the same way, 544 others
    passing; php-async #311 is merged.
18. A failed `fflush` or `fclose` fails the upload: done (#380). A
    `tmp_path_generator` linking to `/dev/full` gives a 16-byte part
    `UPLOAD_ERR_OK` and a 64 KiB part CANT_WRITE with its partial temp file
    kept, 3 of 3; now both read CANT_WRITE with no temp file and size 0, as
    PHP. NO_TMP_DIR has a case too. The generator's header comment now says
    the processor frees the path with its own allocator.
19. Multipart takes memory from `emalloc` in every build: done (#382). The
    processor has no fork left; the parser takes libc only under
    `MP_PARSER_LIBC_ALLOC`, which the PHP-less `fuzz_multipart` sets.
    `multipart/026` (a 4 MiB field held at 3,152 bytes on main, 3 of 3) passes,
    and kills the mutant that keeps the doubled buffer (8.4 MB held). A 40 MiB
    field under `memory_limit=32M`: 503 at the parse, was 500 in the handler.
20. The H3 slot release stops retrying once the reactor leaves RUN
    (`http3_stream.c:123-125` spins with no bound); the slot is dropped with a
    counter. Reproduce through a hook that fails the post, or bound it anyway.
21. `test_parser_security.c` asserts the parse outcome: done. 25 cases asserted
    nothing (23 `(void)result`, plus the missing-Host and CRLF cases under an
    `if` that never ran); each now asserts what was measured, with its RFC
    reason. 18 refuse as RFC 9112 asks (two Transfer-Encoding lines: 400);
    four accept correctly. The fifth, a 50 KB chunk extension, was accepted
    with nothing counting it: #386 caps extensions at 16 KiB per request (413),
    with two boundary cases and `h1/069`.
22. The HTTP/3 cmocka targets run in CI: fuzz-embedded restores the nghttp3
    and ngtcp2 caches and passes `PKG_CONFIG_PATH` to CMake, so
    `HTTP3Packet` and `HTTP3SlotRelease` build; the step fails if any of the
    three HTTP/3 targets is missing from `ctest -N`. `test_http3_packet` reads
    the stateless reset from the send stub: length (41 → 40, 100 → 99,
    1201 and 1500 → 1200), the short-header bits and the token; dropping the
    header-bit fix fails it 3 of 3, dropping the clamp overruns the buffer.
23. Test strength of the static decoders: done. `test_static_decoders` links
    `http_range.c` and checks 18 range headers (clamp, suffix, 416 past the end
    and on an empty file, refusals); a file's ETag changes with its size and
    its mtime; the path rejections have their edges (a length that cuts an
    escape before valid hex, one bad nibble on either side, 256 against 257
    segments, an inner against a trailing empty segment), which kill the `>=`,
    `||` and segment-cap mutants the old cases let through. Non-matching
    If-None-Match and invalid dates were covered already. `static/011`
    compares the range bodies; `compression/076` sends `x-gzip` both ways and
    fails without either alias.
24. The public API does what it says. `setWriteBufferSize`,
    `enableProtocolDetection`, `enableTls` and `setAutoAwaitBody` store a value
    nothing reads; `getTelemetry()` returns `bytes_received`, `bytes_sent` and
    `errors` as a literal 0; `CODEL_TARGET_MS` parses "abc" as 0. Each is wired
    up or removed with a tombstone (CODING_STANDARDS 484-485). First narrow
    `h3/023:34` and `h3/030:41`, which catch Throwable and would count a removed
    setter as rejected.
25. Coverage measures what runs: the ctest run is instrumented and merged into
    the baseline, and `thread_queue.cc` gets coverage through CXXFLAGS.
26. A watchdog is a ceiling, not a wait: the 24 phpt that sleep 1 s or more in
    one call stop the server when the client finishes; tls/007 (15.1 s),
    h2/021, tls/005, tls/015 and tls/009 each drop under 2 s.
27. Workers > 1 on Windows has a test (USAGE.md:688 promises it; 56 phpt skip
    there on SO_REUSEPORT), and static/009 and static/021 name an issue for
    their "tracked gap" skip.
28. Dead code out: the strategy `send_response` and `reset` slots, the unused
    `_http_listen_event_t` and `_http_server_t`, the functions with no caller,
    the SPSC queue with `readerwriterqueue.h`, the unread `HAVE_*` macros,
    `H3_TRACE`, the duplicate CMake entry for `negotiate.c`, and the TODOs
    without an owner; the non-Linux H3 branches are built in one job or removed.
29. Stale text: CHANGELOG 0.15.0 on WebSocket for Windows, `chaos.yml:10`, and
    the step lines of this file left open after their fix ("Compression is not
    wired into the worker path", "#322", "#315") or stale (the Critic note
    of the #311 step says `stop()` closes no connection; #345 changed that).
30. The tests nothing runs: `tests/e2e/*.phpt` (11) move to
    `tests/phpt/server/h1/` and pass there; `tests/fuzz/h3_datagram_fuzz.c`,
    which exercises only ngtcp2 and OpenSSL, is deleted. Edmond, 2026-09-30.
31. A nightly job runs the phpt suite under ASan and UBSan
    (`scripts/build-with-sanitizers.sh`, used by no workflow today), and a
    second pass under `fiu-run` with random `posix/io/*` and `libc/mm/*`
    failures; the pass fails on a crash, a hang or a leak, not on output.
32. Mull on the cmocka binaries, weekly in the fuzz-embedded job, after 22 and
    23 put the HTTP/3 and static targets in the population. The first run
    passes the known-answer check of rule 28.1; the survivors go into
    `dev/HEALTH.md` as the reference run.
33. A connect storm on Windows CI (N parallel connects, time until all are
    accepted), reported and not gating, before and after php-async #307: one
    AcceptEx at a time costs one accept per loop iteration, not measured.
34. A Windows AF_UNIX listener paused with several clients queued (named pipes
    hold up to four pending instances): all are served after the resume, in
    order. The only case where php-async's delivery loop runs more than once,
    so its two outcome branches have no test that kills a mutant of them.
35. A file body that failed or came up short is access-logged and counted as a
    plain 200 with a short byte count (S2.3): mark it failed, as an aborted
    stream is.
36. The HTTP/1 file-body phase has no write deadline: a peer that stops reading
    holds the chain, and with it the connection, for good (Linux: libuv's copy
    loop in `poll(-1)`; macOS: php-async's EAGAIN wait). Critic on php-async
    #308, 2026-09-30. Reproduce with a client that reads 100 KiB and stalls.
37. `multipart/009` failed once on Windows (PR 363, run 36743616544, 1 of 4
    runs of that branch): the third upload's `getSize()` read NULL, an upload
    error. Passed on rerun and on main 8 of 8. Suspect the temporary file
    (item 18's CANT_WRITE path); reproduce under `-j` on Windows.
38. A small php-async PR: regenerate `async_arginfo.h` (its stub hash is stale,
    every build rewrites it), and assert in `sendfile_wait_writable` that the
    destination io has no other sendfile waiting, since `sendfile_waiting`
    holds one (Code Reviewer on S2, 2026-09-30).

- [ ] **Drop the streaming exemption in laravel-spawn.** `TrueAsyncServer::streamContent`
  calls `setNoCompression()` on every `StreamedResponse` as the workaround for #170
  (YanGusik/laravel-spawn#57). Remove it once a build with the flush is in use, and
  re-measure the CSV timeline from the issue.
- [x] **#171 — a streaming response cannot be aborted.** Merged as PR 193 (`cf1c708`). A body truncated by an
  exception read as complete on the wire. `abort` is an op on
  `http_response_stream_ops_t` answering `bool`, and the answer carries the
  distinction the design first missed: false means the transport has not put a
  byte of this response on the wire, so there is nothing to fail and the empty
  response each transport commits lazily is the better answer —
  `sseStart()` followed by a throw keeps its 200. HTTP/1 withholds the
  terminator and drops keep-alive, HTTP/2 sends RST_STREAM(INTERNAL_ERROR),
  HTTP/3 resets the write side with H3_INTERNAL_ERROR, the pool worker posts
  `RESPONSE_WIRE_STREAM_ABORT`, which already existed and was reachable only
  from the two places a wire is lost — an undeliverable post and a credit wait
  that timed out — never from the API. Dispose routes to it on an uncaught exception but **not** on a
  cancellation: the server cancels its own handlers at graceful shutdown, and
  an open feed cut short there has produced every byte it promised.

  Evidence: `h1/033` reads the wire and finds the frames but no terminator,
  `h1/034` keeps the empty-200 shape, `h1/035` proves the pipelined request
  behind a failed stream is not answered, `core/063` walks the state machine,
  `h2/053` reads error code 2 off the RST frame and completes a second request
  on the same connection, `h3/058` and `h3/059` get `RESET(err=258)` out of
  aioquic, `h2/053` also reads back a code the handler named, `grpc/018` keeps
  `grpc-status: 13`, `compression/072` makes the
  decoder refuse the bytes, `h1/036` holds the carve-out — a handler parked in
  a delay and cancelled by `stop()` still gets its terminator, and loses it
  when the carve-out is removed — and `compression/073` runs the whole
  dispose-through-the-wrapper route against a peer that has gone.

  Six defects the work uncovered, all fixed here. **A compressed stream whose
  handler forgot `end()` was undecodable**: dispose finished at transport level,
  below the wrapper that owns the codec trailer, so `gzdecode()` refused a body
  the framing called complete — every dispose path now finishes through
  `response->stream_ops` (`compression/071`). **The pool's abort could ship a
  clean FIN**: the reactor applies a response's wires in one tick and flushes at
  the end of it, so raising the end flag before that flush let the data reader
  emit EOF first; observed once in three runs. **A pipelined request behind a
  failed HTTP/1 stream was still answered**, which put a whole `HTTP/1.1 200 OK`
  where the peer was counting down an unfinished chunk. **A locally sent
  RST_STREAM came back as a peer reset**, cancelling the handler that asked for
  it with "stream reset by peer" and inflating the peer-reset metric. **HTTP/2
  published its chunk ring before the headers it belongs to**, so a failed
  header commit left a ring behind and abort read it as "the peer has heard
  from us". **The pool marked a stream started before its headers wire was
  away**, so an abort was posted against a stream the reactor never opened.

  Two corrections came from review and are carried without a test, which is
  worth naming. The cancel carve-out keys on the exception's class rather than
  on the cancelled flag, so that the parse-error and peer-reset cancels — both
  real truncations — cannot be mistaken for a graceful shutdown; on today's
  paths the peer-closed latches already stop the terminator, and no shape
  reached from PHP tells the two versions apart. And the HTTP/2 wait for ring
  room refuses to park on a peer that has gone, which matters because dispose
  now reaches that wait through the compressing wrapper and a coroutine parked
  there is past cancellation; the ring drains as a handler fills it, so the
  reproduction needs a peer that is simultaneously alive enough to hold the
  window shut and gone enough to be latched, and I could not build one.

## The response body contract (YanGusik/laravel-spawn#50, #60)

Two field reports from one user came from the API rather than from his code, and
the second arrived 44 minutes after the first was closed. `write()` buffers until
`end()`, while Node, Swoole and Go all send under that name; `sendable()` folds
four questions into one bool and `README.md:277` documents it as a liveness check,
which is the loop that truncated his stream, and which laravel-spawn had already
copied into `Sse::connected()`; a handler-declared `Content-Length` reaches the
wire unverified. The contract is settled; the steps are ordered so the
documentation lands first, because the reporter is writing a proxy recipe against
it and expects a tag within days.

- [x] **Document the three body modes first.** `docs/USAGE.md` says nothing about
  the response body at all. It gains a section naming the modes — buffered
  (`setBody`), streamed (`write`), file (`sendFile`) — the state each commits, and
  a framing table: a buffered body gets its `Content-Length` computed, an
  undeclared stream is chunked or DATA frames, a declared stream keeps the header.
  `README.md:277` and the `write()` docblock (`stubs/HttpResponse.php:160`, which
  never says that nothing leaves before `end()`) are corrected in the same step.
  Done in #174: the README guard is gone and both docblocks say what they mean.
  The `docs/USAGE.md` section landed with the renames as §3.5, written once
  against the final names.
- [x] **`isWritable(): bool` — liveness, with the op behind it.** A new optional
  `is_alive` in `http_response_stream_ops_t` (`include/php_http_server.h:706`);
  every backend already computes it inside `append_chunk` (`peer_closed` for H2,
  `stream_credit_is_dead` for the worker). Sound as a predicate because every
  input is a one-way latch, unlike queue depth.
- [x] **`write()` becomes the streaming call.** Done in #180. `send()` is removed
  outright rather than kept as a deprecated alias: it would have covered one call
  in the shipped laravel-spawn adapter while `isClosed()` breaks three others
  beside it (`src/Server/TrueAsyncServer.php:106,395,413,492`), so the adapter
  needs a release either way. Buffered incremental appending keeps its behaviour
  under `appendBody()`; `isClosed()` became `isEnded()`;
  `sendable()` throws a tombstone naming `isWritable()` and `tryWrite()`;
  `setBodyStream()`/`getBodyStream()` are deleted. Evidence:
  `tests/phpt/server/core/062-body-api-names.phpt` reads the wire for each mode,
  and `h2/023-h2-sendable-tombstone.phpt` asserts the throw on a live H2 stream
  where the method used to answer. `docs/USAGE.md` §3.5 documents the three modes.

  Two defects surfaced while doing it, both fixed here. **#181**: a buffered body
  followed by a streaming call was discarded with no error — the streaming path
  never reads `response->body`, while the reverse direction has always thrown, so
  only one side of a symmetric-looking mistake reported. **The `stream` perf
  profile had never run**: `tests/perf/servers/server_stream.php` called `->send()`
  with no argument against an arginfo requiring one, so the profile answered 500
  with `expects exactly 1 argument, 0 given` before measuring anything, and its
  chunk loop buffered through the old `write()`.
- [x] **`tryWrite(): bool` and the dialect twins.** In #178, without the twins. The non-blocking half of the
  pair, matching `WebSocket::trySend()`; `trySseEvent()` and `tryWriteMessage()`
  follow, so the idiom is not half-applied. Invariant: false means nothing was
  queued and no header was committed, and a dead peer is still the 499 exception.
  Blocked by the compressing wrapper — `ws_append_chunk` feeds the encoder and
  closes a block before it consults the underlying ops, so a refusal there is not
  retryable, and the capacity check has to move ahead of the encoder.
  Three review passes reshaped it. The refusal moved into `append_chunk` as a
  `nonblocking` argument, because a predicate read beforehand cannot be atomic at
  the PHP boundary; the wait moved into a `wait_writable` op, because each
  transport's own wait carries a deadline, a wake source and a re-pump of the
  drain that a wait assembled outside would drop; `awaitWritable()` answers false
  rather than true where a transport can be full but offers no wait, since "go
  ahead" spins a handler that trusts it. `compressing_stream_ops` and
  `h3_stream_ops` gained the missing `sendable` slots — without them a refusal
  under compression threw away a block the encoder had already emitted, and the
  retry the caller was told to make corrupted the deflate stream.

  **The HTTP/1 exception is closed** (#179, PR 214). Not by moving the path off
  the await, which the measurement refused to support, but by splitting the two
  callers: `write()` keeps the awaited sender and with it the return value that
  sets `stream_dead`, the bracketed deadline and the copy-free frame, while
  `tryWrite()` queues on the connection's tail and is refused once the outbound
  depth — the tail plus the write in flight — reaches the high-water mark.

  Counting the write in flight is what makes the refusal honest: the tail alone
  reads zero for the whole first write, so the answer arrives one write late and
  the memory a slow peer pins is twice the knob. Two things the await had been
  providing came with the queued path — the read side latches `write_failed`,
  because libuv reports a failed write at completion and a fire-and-forget
  completion carries no status, and the connection's write deadline is armed at
  submit, because queued bytes have no await to time out.

  Evidence: `h1/053`, restored from the commit that deleted it in #177 — 42
  chunks accepted against a peer that never reads, the 43rd refused, the handler
  still running. `h1/032` now answers a refusal with `awaitWritable()`: its 499
  rested on the hidden park this step removes.
- [x] **The dialect twins: `trySseEvent()` and `tryWriteMessage()`.** The idiom is
  half-applied while `tryWrite()` has a non-blocking form and the two dialects do
  not. Shape: one static helper per dialect carries the formatting and the guards,
  and the blocking and non-blocking entry points differ only in the flag they pass
  to `append_chunk` — `sseEvent()` and `writeMessage()` move onto it rather than
  keeping a second copy. Both new methods answer `bool` on the `tryWrite()`
  contract: false means nothing was queued and no header was committed, a dead peer
  is still the 499 exception, and HTTP/1 never refuses because it has no queue to
  refuse from. Evidence to produce: phpt on H1 and H2 that a refusal leaves the wire
  untouched, that the bytes match the blocking twin, and that the peer's death still
  throws; `docs/USAGE.md` §3.5 and a CHANGELOG entry.

  Done. Evidence: `h1/031` shows HTTP/1 accepting every offer and the record matching
  `sseEvent()` byte for byte; `h2/026` and `h2/027` fill the ring, take a refusal, wait it
  out and end with a body identical to the blocking twin's — `027` alternates the two
  twins, so the same body also proves they frame alike; `h1/032` aborts the peer and gets
  499 out of both, on a response that is still uncommitted.

  Four defects surfaced and are fixed here. `awaitWritable()` was refused in SSE mode by
  the guard that keeps `write()` out of it, so the handler the twins were written for had
  nowhere to wait — waiting emits nothing, so that guard and the buffered-body one now
  apply only to the calls that emit. `writeMessage()` never answered to those guards at
  all: a gRPC frame could be pushed into an SSE body or discard a buffered one, and the
  refactor would have copied the hole onto a second public method. `trySseEvent()` started
  the stream before probing the peer, so a 499 left the response committed and the handler
  could not answer with a status. And `awaitWritable($timeoutMs)` documented a deadline no
  transport read: HTTP/2 now waits the shorter of it and the connection's write timeout.
- [x] **Framing by declared length.** Merged as PR 195 (`d44e749`), two commits:
  the step, and the answer to its review. A `Content-Length` set before the first
  `write()` reaches the client verbatim on every protocol, and the server becomes
  the auditor: excess throws at the offending write, a shortfall aborts the stream
  at `end()` instead of finishing cleanly under a header that lies. Compression is
  disabled for such a response. `src/http1/http1_format.c:249` is the only path
  that passes a handler value today; H2, H3 and the worker strip it in
  `http_response_header_allowed_h2h3`. The abort op it waited on landed with #171.

  The declaration is taken at the first `write()`, into `declared_length` on the
  response, and the count is **reserved** before each chunk reaches the transport
  rather than added after it returns: every `append_chunk` suspends, so a second
  coroutine writing to the same response would otherwise be admitted against a
  total already spoken for. The shortfall verdict sits in
  `http_response_finish_stream`, the one finisher `end()`, `abort()` and all four
  dispose paths pass through, and it compares for equality — an overshoot the
  reservation cannot produce today would be failed too rather than trusted.

  Four carve-outs decide what declares. **gRPC** closes its body with a trailer
  frame the handler never sees, and `grpc_call_finish` is a fifth finisher that
  bypasses `finish_stream` — a declared length there would be carried and never
  audited, so `grpc_mode != 0` declares nothing. **SSE** starts its stream
  through `sse_ensure_started` and keeps its framing. **HEAD** returns before
  the guard, so on HTTP/1 the length stays on the buffered path where it
  describes the body a `GET` would return; on HTTP/2 and HTTP/3 it is stripped
  there as it always was, which is the open step below. And a status defined to
  carry no content (`1xx`, `204`, `304`) declares nothing, so such a response
  keeps the framing it had — chunked, which RFC 9112 §6.1 forbids on it just as
  it forbids the identity body: the carve-out declines to widen that defect
  rather than closing it, and it belongs to the HTTP/1.0 step below.

  A declaration is not always what reaches the client. When the body ends short
  and `abort()` answers false — nothing on the wire — the finisher rewrites the
  header to the count actually written, because the empty response committed
  behind it would otherwise leave the peer counting down to bytes that are not
  coming.

  What a declared stream must not do is let bytes past the guard. `write()`,
  `tryWrite()`, `end($data)` and both message calls go through it;
  `writeMessage()` needs no gRPC mode of its own, so on a plain request it would
  otherwise have put an uncounted frame onto an identity-framed body — found by
  the intent reviewer on the finished diff, fixed here, pinned by `core/064`
  route `/frame`. A declaration is therefore taken by whichever streaming call
  comes first, and the byte counted is the one that call puts on the wire, not
  the payload it was handed.

  The reservation is given back whenever the transport queued nothing — a
  non-blocking refusal, and every `HTTP_STREAM_APPEND_STREAM_DEAD`. Keeping it
  on a dead stream let a body that never reached the peer satisfy the count, so
  HTTP/2 ended cleanly under a `content-length` that overstated it. And the
  declaration is adopted only once a chunk is accepted: a first offer refused
  for over-run leaves the response uncommitted and free to answer buffered, and
  a length recorded before that check reached the HTTP/3 field section beside a
  body of another size. Both found by the adversarial reviewer on the finished
  diff.

  Two limits are known and carried without a test. The header correction on the
  abort-answers-false branch needs a transport that refuses its first chunk
  before opening the stream, which no shape reachable from PHP produces today.
  And on the pool path the worker counts a chunk once the wire is posted, while
  the reactor may drop it for a stream that has moved on (`http3_dispatch.c`
  `RESPONSE_WIRE_STREAM_CHUNK`) — the count is a floor there, so a declared
  length can be reported kept when the reactor discarded part of it.

  Evidence: `h1/037` reads nine body bytes with no framing around them and
  completes a second request on the same connection, `h1/038` refuses the
  offending chunk and still finishes the promised body, `h1/039` fails a short
  one, `core/064` walks the declaration's edges from PHP — malformed, twice,
  late, exact — and proves the malformed case leaves the response uncommitted
  and answerable with a 500, `h2/054` and `h3/060` read the field off the wire
  against an undeclared control and get a reset on a short body, `h3/061`
  carries it across the pool wire, `compression/074` shows the same payload
  gzipped without a declaration and identity with one.

  Two defects the work uncovered. **A handler `Content-Length` that disagreed
  with a buffered body desynced the connection**: the value was emitted verbatim
  next to whatever body the response held, so eleven bytes under a header naming
  five left the client reading the last six as the next status line. The server
  now states the count it is sending, except on `HEAD`, and `core/064` route
  `/buffered` pins it. And **one predicate dropped both framing headers**, so keeping `Content-Length` on the streaming
  path would have kept a handler's `Transfer-Encoding` with it — the pair RFC
  9112 §6.1 forbids and §6.3 names as the smuggling shape; the two names are
  dropped separately now.

  Reviewed by four critics against the design before any code: they found the
  gRPC finisher bypass, the commit-then-throw ordering, the
  two-headers-one-predicate lever, the HEAD claim that held for `write()` only,
  and the check-then-count race. Reviewed again on the finished diff, which is
  where the `writeMessage()` hole and the missing test for the buffered fix came
  from. Each finding is fixed above or an open step below.

- [x] **#197 — an HTTP/1 message carries the body its status and method allow,
  framed the way the request can read.** Merged as PR 199 (`6892b97`), eleven
  commits: three for the step, six answering two rounds of critic review, one
  for a pass over every comment it adds. The step opened as one defect and the
  critics turned it into six, all of the same shape: the response frames itself
  from the declaration alone and reads neither the request version, nor the
  method, nor its own status.

  One predicate now answers for all of it — `h1_response_framing` in
  `src/http1/http1_format.c`, four values from RFC 9112 §6.3's eight receiver
  rules: none (rule 1), length (5), chunked (3), close-delimited (7). The
  streaming formatter and the stream vtable read it. The buffered formatter
  states a length of its own, so it answers the same two questions — does this
  status carry a body, does this method — from the same inputs rather than from
  the enum; what is gone is the third place that used to decide, not the
  second.

  **HTTP/1.0.** `Transfer-Encoding: chunked` went to every 1.0 client, which has
  no decoder for it (§6.1). Such a body is delimited by the close now, with
  `Connection: close` in the header block. That costs the connection, so a
  pipelined request behind it goes unanswered: `ctx->close_delimited` joins
  `stream_dead` in the `framing_lost` gate, without which the drain answers the
  next request *into* the body and `http_connection_on_request_ready` re-raises
  `conn->keep_alive` from it, so the EOF that ends the first response never
  arrives. Two critics found that independently.

  **The connection was never told how it ends.** A streaming response committed
  its header block at the first `write()` and the `Connection` decision was
  taken in dispose, after those bytes had left — so a request that asked for
  close and a graceful drain both went unmentioned to every streaming client.
  The decision moved into `h1_streaming_headers_build`, which is the only place
  a streaming response can state it. Dispose still asks, because a stream
  outlives the answer — see the correction below. The 1.0 keep-alive echo
  (RFC 2068 §19.7.1) lands in the same place and in dispose for the buffered
  path — without it "a 1.0 client that declares a length keeps keep-alive" was
  server-side fiction.

  **A status that carries no body.** Deferred here by the declared-length step.
  A streaming call throws while the response is uncommitted; a buffered body is
  dropped at format time, because the status may legitimately be chosen after
  the body was built and there is no one left to tell. `sseStart()` refuses;
  a 304 keeps a handler `Content-Length` (RFC 9110 §8.6) and a 1xx and a 204
  lose it (§8.6). The refusal is in the shared `write()`, so it reaches HTTP/2
  and HTTP/3 too.

  **`HEAD` shipped a body** through `sseEvent()` and `writeMessage()`, which
  never had the drop `write()` and `tryWrite()` do.

  **The buffered path sent `Content-Length` beside a handler
  `Transfer-Encoding`** — the pair §6.1 forbids and §6.3 names as the smuggling
  shape. The streaming path stopped in #195; the buffered one drops it now.

  **The buffered drop was HTTP/1's alone, while the docs and the test said it
  was everyone's.** Found by three of the four critics on the finished diff and
  confirmed on the wire with this repository's own HTTP/2 client: a 204 with a
  buffered body put `DATA("oops")` on the wire, a 304 did the same, and a
  `HEAD` shipped the whole body — the last one on `main` too, since HTTP/2 had
  no `HEAD` suppression at all. The drop now sits where each transport reads
  the body for the wire, in `http_response_get_body` and
  `http_response_get_body_str`, so HTTP/2, HTTP/3 and the pool worker inherit
  it from one place and HTTP/3's own `is_head` branch is gone. `h2/055` had
  been reading the body with curl, which discards content on a 204 whatever the
  server sent; it counts frames now and asserts no stream was reset, so an
  empty body proves the server dropped it rather than the client hiding it.

  **Five defects the review found and this step fixes.** Recorded together
  because they are one family — the server states a message's framing, and each
  of these was a place where it did not.

  **A header value reached the wire unfiltered**, which is #198's other half
  and the reachable one: `setHeader('Location', $userInput)` with a CRLF ended
  the header block and put a second response behind it (CWE-113). `setHeader()`,
  `addHeader()` and `redirect()` refuse a name that is not an RFC 9110 §5.6.2
  token and a value carrying a byte §5.5 does not allow. Refused rather than
  cleaned, because these run while the handler can still answer; `redirect()`
  checks before assigning the status, so a refused call changes nothing.
  Evidence: `h1/046`.

  **A handler `Connection: close` was copied and ignored**: the field reached
  the peer while `conn->keep_alive` stayed true, so the next request was
  answered on a socket the client had retired, and on 1.0 the keep-alive echo
  overwrote the handler's value instead. The field is taken as the request it
  is now — the connection closes, and the peer is told by the branch that
  already tells it about a drain. **`Transfer-Encoding` was dropped whatever it
  named**, so a handler that set `gzip` sent encoded bytes with no coding
  declared; only `chunked` is accepted and dropped now. Evidence: `h1/047`,
  which writes a second request onto the same socket — the header alone never
  proved anything.

  **A 1xx framed correctly and left the exchange unfinished.** It is an interim
  response (RFC 9110 §15.2) and nothing follows it, so both ends waited for the
  other; to a 1.0 client, which §15.2 forbids it to entirely, it read as the
  answer. `setStatusCode()` takes 200 to 599 now. `core/027` pinned the old
  boundary and changes with it — the one contract in this step that was already
  written down, and worth naming for that reason.

  **205 carried a body**, which RFC 9110 §15.3.6 forbids, and could not have
  framed one: §6.3 rule 1 does not name 205, so a client still looks for
  framing. Its own leg — body dropped, `Content-Length: 0` stated — rather than
  joining 204 and 304, which state no length at all.

  **A `HEAD` whose handler streamed reported `Content-Length: 0`**, because
  `write()` returned before the stream committed and the buffered path computed
  a length from an empty buffer. The stream commits now and only the bytes are
  dropped, which also makes the four streaming calls agree with the two SSE
  ones.

  **A second review round, and eight more findings.** The six commits above
  went back to four critics, and what they returned is recorded here because
  one of it was a regression this step had just introduced.

  **Committing a HEAD stream threw the handler's failure away.** The first
  shape of the HEAD fix called `http_response_stream_commit_once`, which sets
  `committed`, and the dispose path derives a 500 from an uncaught exception
  only while the response is uncommitted. A `HEAD` handler that wrote a chunk
  and then threw answered `200 OK` with a clean header block — and on a message
  that ends at the header block a failed response is byte-identical to a
  successful one, so the peer cannot tell. Health checks read it as green.
  Confirmed on the wire before the fix. The commit is gone: a dropped chunk
  records `head_streamed` and nothing else, so the response stays uncommitted,
  `setHeader()` goes on working, and the buffered formatter reads the flag
  instead of measuring an empty buffer. That also closed three findings the
  same commit had opened — `end($data)` reaching `append_chunk` on a HEAD over
  HTTP/2, a HEAD losing the ability to state a length it measured, and the pool
  worker reverting such a response to a buffered wire.

  **`sendFile()` and `json()` wrote `status_code` past `setStatusCode()`.**
  `new SendFileOptions(status: 204)` framed a whole file under a status the
  client ends at the blank line; `json($d, 100)` produced the interim response
  that hangs the exchange. One predicate answers for all three now, and
  `sendFile()` also refuses a status defined to carry no content.

  **An object value walked past the header check.** It tested the zval's type
  and skipped anything that was not a string, while storage converts — so a
  PSR-7 URI built from a query parameter, which is the commonest way request
  data reaches `Location`, went through unchecked. The bytes checked are the
  bytes stored now. Leading and trailing whitespace is refused in the same
  place (§5.5), and `setTrailer()` answers to the check as well: HTTP/1 emits
  no trailers today, but gRPC puts an exception message into `grpc-message`,
  and the day a chunked-trailer emitter lands there that is CWE-113 with
  nothing in its way.

  **`Connection: keep-alive` was refused where a drop was right.** The shape
  that sets it is a handler copying an upstream response's headers wholesale,
  and refusing it turns a correct response into a 500 over a field the server
  ignores. It is dropped; `close` is still read; anything else still throws.
  `resetHeaders()` clears the recorded close, which it did not, and the flag is
  documented as HTTP/1-only, which the first shape of it was not.

  **Compression did not know about 205**, so a 205 with a compressible body got
  `Content-Encoding: gzip` beside `Content-Length: 0` and no bytes. It reads
  `response_status_carries_body` now, so the list cannot drift again.

  **A drain that came due mid-stream was lost.** The first shape of this step
  gated dispose's whole `Connection` block on `http_response_is_streaming`, to
  keep the drain evaluator from being asked twice for one response. The
  evaluator is idempotent once latched — both its arms are guarded, and the
  second call returns the same verdict and increments nothing — so what the
  gate bought was nothing, and what it cost was every drain that arrives after
  the header block: a connection that reached its age limit while its body was
  going out kept `keep_alive` and was handed on for reuse, which `main` did not
  do. Dispose asks again now. What a streaming response does not do there is
  touch the header block — those bytes may already have left — so the telling
  and the counter stay with `h1_streaming_headers_build`, which asks again and
  sees the same latched verdict. Evidence: `h1/045`, and `h1/044` reads the two
  counters exactly rather than as floors, which is what would catch the double
  count the gate was there to prevent.

  Evidence: `h1/040` reads the 1.0 body with no framing around it against a 1.1
  control, `h1/041` proves the pipelined request is neither answered nor run,
  `h1/042` reads the keep-alive echo back and completes a second request on the
  socket, `h1/044` reads the drain's `Connection: close` off a streamed answer,
  `core/065` walks the bodiless-status contract from PHP on both paths and the
  `HEAD` dialect, `h1/045` proves a drain that comes due mid-stream still
  retires the connection, `h2/055` reads the frames HTTP/2 puts on the wire for
  each bodiless shape, `compression/075` decodes a gzipped close-delimited body
  and holds the three rules that meet on a declared 1.0 stream, `tls/016`
  drives the identity write branch over TLS with a chunk above the coalescing
  bound, `h1/046` refuses a header that would split the message and `h1/047`
  proves a handler `Connection: close` closes the socket, and `sendfile/004`
  refuses a status a file body cannot go under. Fourteen of the fifteen tests
  this step touches fail against `main`'s sources and pass here — the whole set,
  including the three that existed before it (`core/027`, `core/036`,
  `sendfile/004`). The exception is `h1/045`, which passes on `main` as well,
  because the defect it guards was introduced and removed inside this branch.

  Reviewed by four critics against the design before any code: 32 findings, 12
  survived verification, every one of them fixed above. The RST findings were
  refuted and are worth recording — a plain HTTP/1.0 request already closes the
  connection today, so close-delimited framing changes no byte of that path.

- [x] **#198 — an exception message reached the HTTP/1 status line unfiltered.**
  Merged in the same PR 199. Found while reading `emit_status_line` for the step
  above. A CRLF in the
  message ended the status line, so the bytes behind it were read as header
  fields and, past a blank line, as a second response (CWE-113) — reachable from
  any handler that puts request data into an exception message, and from
  `setReasonPhrase()`. The phrase now keeps only what RFC 9112 §4 allows;
  everything else becomes a space. Cleaned rather than refused, because
  `reset_to_error` runs while an exception is already being handled. Evidence:
  `h1/043`. Not addressed: the phrase still carries the whole message, so a 500
  puts application text of any length on the status line.

- [x] **#202 — the static path advertises `Connection: keep-alive` on HTTP/1.1
  too.** Three sites, not one: the served file (`send_file.c:455`), the inline
  error (`:236`) and the 416 (`:372`) all stated the field from the connection's
  verdict alone. The rule now lives in `h1_response_state_connection` beside
  `h1_response_peer_speaks_http11`, and all five callers read it — the three
  static ones with the real verdict, the two handler ones with `true`. The
  version check was written twice before and statics would have been the third
  copy. The wrong CHANGELOG claim went with the pass that shortened the whole
  `[Unreleased]` block. Evidence: `h1/048` reads the raw wire on four requests
  and fails against `main` on two of them.

- [x] **#200 — HTTP/2 and HTTP/3 strip `Content-Length` from every response,
  `HEAD` and static files included.** Answered A of the two readings: every
  buffered response states a count, rather than only the `HEAD` and the
  `sendFile()` whose framing cannot answer for itself. `sendFile()` is framed by
  DATA and END_STREAM as completely as any buffered GET and sat in the narrow
  answer for a download-sizing reason that holds for every body, so the narrow
  answer had no principle to stand on. The step was not four `false` arguments:
  the rules lived inside `http1_format.c`, and `head_streamed` inside
  `http_response_internal.h`, where no HTTP/2 or HTTP/3 site could read them.
  They are now `http_response_length_action` with four outcomes — state the
  buffer's count, state zero, keep the field the table holds, send none — and
  HTTP/1 reads it instead of its own copy. The one fact no call site had is a
  bit, `length_stated`: the static engine writes a count into the table while
  the buffer stays empty, so `http3_stream_submit_response` could not tell that
  count from a handler's. Two rules the reading missed: a response carrying
  trailers states nothing, because nghttp2 ends the stream at the byte
  completing the count and the trailing HEADERS then reaches it closed
  (`h2/003` caught it); gRPC needs no carve-out, since `writeMessage()` puts the
  response in streaming mode and the streaming rule already covers it.
  Evidence: `h2/056` and `h3/062` are new and fail against `main`; `static/012`
  now reads `cl=4096` on GET and HEAD, `h3/019` counts 72 headers. 455 phpt,
  429 passed, 0 failed, 24 skipped on absent tool gates, 1 warn (`core/047`,
  the pre-existing XFAIL that passes); `ctest` 16 of 16.

- [x] **#206 — the access log reports 0 bytes for every `sendFile()` body and the
  unencoded size for every compressed stream.** Every transport now reports the
  octets it put on the wire through `http_response_add_sent_bytes`, and the
  response keeps them in `transport_body_size`, separate from `written_length`
  because the declared-length audit compares that one with what the handler
  promised. Two things the report got wrong, both found by writing the test. The
  file half is narrower: a file at or under the 64 KiB slurp threshold is read
  into the response body, so a small asset was logged correctly all along, and
  the defect is larger files and ranges. And the HTTP/1 count did not exist — the
  claim that all three pumps count holds for HTTP/2, HTTP/3 and HTTP/1 over TLS,
  while the plain kernel path moves the slice in one `uv_fs_sendfile` and
  finalized without reading the result; it now adds `req->transferred`.
  Evidence: `core/067` covers HTTP/1, HTTP/2 and a gzipped stream, and fails
  against `main` on all three. HTTP/3 takes the same reporting call and has no
  test of its own.

- [x] **Measure what the framing work costs per response.** Taken on 2026-08-22,
  `b0ca84c` against `ebbe410` on a release build: **126 ns per response**, 3.3% of a
  3-byte HTTP/2 one, and #200 is the slower of the pair in 14 rounds of 16. The
  comparison had to be paired — one build's own spread is 6%, so two independent
  medians put the effect inside the noise. `http_response_commit_content_length`
  writes the count into the header table, so every buffered response pays a
  `zend_hash_update` plus two `zend_string` allocations — the lowercased name and
  the formatted value — where the field used to be dropped and cost nothing. HTTP/1
  is untouched: it reads the action and formats into the smart_str as before. Two
  things the measurement does not cover: HTTP/3, which calls the same function and
  therefore spends the same 126 ns against an unmeasured baseline, and a large body,
  where a 66% run-to-run spread at 64 KiB drowns a 3% effect. #195 and #197 are not
  measured at all. Evidence: `dev/BENCHMARKS.md`, 2026-08-22.
- [x] **#235 — hand the Content-Length to the flatten loop instead of the header
  table.** The count reaches the wire as an entry of its own, the way `:status`
  already did, so a buffered response stops paying a `zend_hash_update` and two
  `zend_string` allocations for a field no PHP code reads back.
  `http_response_wire_content_length` writes the digits into a buffer the caller
  owns and answers how many, with a second output for whether the table's own
  field is copied instead. The four submit sites read it: HTTP/2, HTTP/3, the
  static HTTP/2 engine and the reactor pool wire. The two
  streaming sites keep `http_response_keeps_declared_length`, because a stream's
  count is the handler's own field in the table and the status rules that the
  buffered path applies first would answer differently for a streamed 204 or 205.

  Measured the same way, paired rounds on `/b3` against a release build:
  **95 ns per response**, 3.6%, 15 rounds of 19, and the order of the two builds
  flipped every round so the first-position penalty falls on both. The insert was
  measured at 126 ns from the other side. Evidence: `dev/BENCHMARKS.md`,
  2026-08-24; 351 of 371 phpt (20 skipped, 0 failed), `ctest` 16 of 16.

  No test is new. The change moves where the field is written and not what goes
  on the wire: `h2/056`, `h3/068` and `static/012` read the count back off the
  wire and `h3/019` counts the fields that go out with it, all four passing here.
  The reactor pool wire has no
  end-to-end coverage of its own here — the suite exercises it through the
  self-test hooks — so its copy is carried on the build alone.

- [x] **#204 — an aborted request is logged and counted as the status it
  committed.** Answered by looking at what other servers do rather than by
  taste: nginx keeps `$status` and reports completion in `$request_completion`,
  Apache in `%X`, Envoy in `%RESPONSE_FLAGS%`, HAProxy in `termination_state`.
  None substitutes the status, so ours stays what the peer was told. The record
  is OTel Logs with semconv names, so the marker needed no invented key —
  `error.type` is conditionally required exactly when a request ends in an error
  and forbidden when it does not. `responses_aborted_total` joins the counter
  table, overlapping the four status buckets rather than joining them.

  The second column went with it. `written_length` turned out to count only a
  declared stream's bytes — the reserve was below the `declared < 0` return — so
  the plan's claim that it "holds the real number" was true for one case in two.
  It now counts every chunk, the audit still reads it only against a
  declaration, and `http.response.body.size` reports it. Evidence: `core/066`
  fails against `main` on all four of its lines, including a 204 logged with the
  four bytes the wire dropped. Not addressed: a `sendFile()` body never passes
  the response object, so its size stays 0, and a compressed stream reports what
  the handler wrote rather than the octets on the wire.

- [~] **Migration.** The CHANGELOG entries are written (#180, five bullets covering
  the seven renames, plus #181 under Fixed). What is left is laravel-spawn, and it
  is five call sites rather than the two this plan assumed: `send()` → `write()`
  at `src/Server/TrueAsyncServer.php:492`, `isClosed()` → `isEnded()` at 106, 395
  and 413, and `Sse::connected()` → `isWritable()` at `src/Sse/Sse.php:42`. To land
  once a build carrying the renames is tagged. Its docblock was
  corrected ahead of the rename in YanGusik/laravel-spawn#63, so the wording stops
  teaching the loop that truncated #60 in the meantime.

## HTTP/1 has no non-blocking write, and the two candidate fixes are both wrong

`tryWrite()` cannot refuse on HTTP/1: the streaming path writes through
`http_connection_send` → `send_raw`, which submits a `uv_write` and awaits it, so
backpressure is the kernel socket buffer and there is no depth to read. Two
designs were worked out and both fail on something mechanical.

- **A second writer for the non-blocking case** (`http_connection_send_batched`,
  the one WebSocket uses). It is an unordered channel: a chunk body parked in
  `out_pending_buf` waits behind an in-flight write while the headers, a blocking
  `send()` and `mark_ended`'s terminal chunk go out through the raw path and reach
  the peer first. Chunked framing does not survive that.
- **A queue on the response** (`http1_request_ctx_t`). Dead twice over: on TLS the
  drain writer would run in scheduler context, where `tls_wait_space` refuses
  outright (`src/core/http_connection_tls.c:97`), so it could not push a byte on
  HTTPS; and the context is freed in `http_request_finalize`, while a queue drained
  by write completions outlives the handler by definition. The precedents cited for
  it — the H2 per-stream ring and the wslay FIFO — both live on connection-lifetime
  objects, not on a per-request one.

- [x] **Measure the HTTP/1 chunk path before deciding.** Taken on 2026-08-20, on a
  release PHP built for it (`dev/BENCHMARKS.md`). `strace` confirms three `write(2)`
  and three loop turns per chunk, flat in the chunk size. A one-hunk prototype that
  copies the three pieces into one awaited write gains 42.7% at four chunks, 77.3%
  at sixteen and 84.8% at sixty-four, cutting the per-chunk cost from ~18.5 µs to
  ~8.8 µs. The copy is only there because the ABI has no awaitable vectored write:
  `io_pipe_writev_cb` sends no NOTIFY and frees the request itself.

  What it decides: the win needs no queue, no second writer and no per-response
  structure, so it is not an argument for #179.

- [x] **Send a streamed chunk as one write below 32 KiB.** Merged in #184. The
  threshold is where the copy stops paying, measured: +25% at a 32 KiB chunk, −16%
  at 64 KiB, so a larger frame keeps the copy-free three-write path. Verified
  against a 9% noise floor by alternating the two builds — +81% at 4 KiB chunks,
  and no difference at 64 KiB, where both take the same path. The bound is on the
  frame rather than the chunk: `HTTP_TLS_PLAINTEXT_RING_BYTES` is also 32 KiB, so a
  chunk-sized bound made a 32 KiB chunk spend a second ring cycle on a TLS record
  carrying six bytes, and a static assert now keeps the two from drifting.

  The header block rides inside the first frame when the two fit: +28.5% on a
  one-chunk response, which is the shape of an SSE reply. `mark_ended` checks
  `stream_dead` before its header commit, so a stream that died after its headers
  landed no longer sends them twice.

  The copy stays until ext/async grows a vectored write that reports its status.
  Its release callback carries none today (`zend_async_API.h:588`) and
  `io_pipe_writev_cb` destroys the exception before calling it, so a coroutine can
  be resumed from there but cannot learn whether the write failed — and on this
  path a failed write is what makes `isWritable()` honest (#176). The same change
  would close the buffer-lifetime hazard both branches carry: libuv keeps the
  caller's pointer until its completion callback, while a cancelled request is only
  marked pending.

- [x] **A cancelled write no longer outlives the buffer it points at.** Merged as
  server#186, on true-async/php-src#24 (+#25 into stable) and php-async#262.

  libuv does not copy: `uv_write` keeps the caller's pointer until its completion
  callback, and the caller freed it when *its own wait* ended — the same moment
  until a cancellation, after which the wait is over and the write is not.
  Reproduced with a peer whose `SO_RCVBUF` is 4 KiB and never reads,
  `setShutdownTimeout(0)` so `stop()` skips the grace window and cancels at once,
  and a streaming handler: the await returns incomplete, dispose defers because
  the write is in flight, and the caller frees a 20008-byte frame that the
  allocator immediately hands back to the next request. The disclosure itself was
  not demonstrated — the connection closes before the loop writes again — so this
  is a dangling pointer, not a shown leak.

  The ABI grew one bit: `ZEND_ASYNC_IO_WRITEV_AWAIT`, in the flag word the
  vectored write already had. The reactor keeps the request alive and notifies
  `io->event` with the request as the result and **no exception** — passing one
  would wake every reader and writer on the handle, because each listener
  forwards an exception unconditionally. On plaintext the HTTP/1 frame is now
  slots the reactor owns, so the body is not copied either; TLS keeps the copy,
  since `tls_push` copies into the BIO ring and a vectored write at the socket
  would put plaintext on a TLS connection.

  Two defects the same work uncovered: `h1_emit_headers_once` and the
  empty-first-chunk branch had the identical lifetime bug and now go through one
  helper; and `http_handler_coroutine_dispose` sealed a frame a cancellation cut
  in half, because it skips `mark_ended` and so skipped the `stream_dead` guard
  `4c7824c` added — the next request on that connection desyncs. macOS caught it
  where Linux could not: there the write still lands.

  Guarded by `ZEND_ASYNC_API_VERSION_NUMBER >= 0x001900`, not by the macro name.
  The name says the header knows the flag; only the version says the reactor
  keeps it, and a build pairing a new header with an old reactor hung — which is
  exactly what CI did in the window between the two merges.

- [x] **Cover the three files the #177 work left behind.** The coverage gate on
  #182 flagged a drop inherited from #178 — the compressing wrapper's four
  delegating ops, the pool worker's credit-backed answers, and the HTTP/1 branches
  around the header block. `compression/052`, the trio added to `h3/047`, and two
  more shapes in `h1/029` close it: compression 75.68 → 78.38, worker_dispatch
  76.63 → 79.62, http1_stream 66.23 → 71.43.

- [x] **Measure what a chunk costs before deciding.** Taken 2026-08-20, in
  `dev/BENCHMARKS.md`: one `writev` and one park per streamed chunk on plaintext HTTP/1,
  flat from 4 KiB to 64 KiB, and half a write with a seventh of a park on TLS, where the
  BIO ring already batches. The three submits the case rested on are one, so what a queue
  could still remove is the single park — and only by making the write fire-and-forget,
  which leaves `isWritable()` and `tryWrite()` with nothing honest to answer.
- [x] **Answer from the queues the connection already has.** Reread against the
  code on 2026-08-21, and the premise is gone. The step said the number behind it
  had never been taken; it was taken on 2026-08-20 and is in `dev/BENCHMARKS.md`
  — three writes per HTTP/1 chunk cost about 10 µs, and sending the frame as one
  write is worth up to +85% rps. The change that measurement supported has landed:
  `h1_stream_append_chunk` sends the plaintext frame as owned slots through
  `http_connection_send_strv_awaited`, and the three-write path survives only for
  a TLS chunk past `H1_CHUNK_COALESCE_MAX`. What the measurement explicitly does
  not support is the queue: "the win is reachable without a queue, without an
  ordering hazard between two writers and without a per-response structure".

  What was left of the step was one thing it listed as a prerequisite: two
  writers reach the socket without consulting the pending tail. The path was
  established the same day and turned out to need no WebSocket at all — the
  HTTP/1 dispose itself uses both senders, picking by body size, so three
  pipelined requests answered small, small, large came back 1, 3, 2. Filed and
  fixed as #209, and the parse-error responder went with it: the same three
  requests with the last one malformed answered `200, 400, 200`, because that
  4xx is built in the read path and was written at the socket from there. Its
  plaintext side now queues; TLS keeps the BIO ring. The awaited senders
  (`http_connection_send_raw`, `http_connection_send_strv_awaited`) read
  neither flag either, and the reproduction that shows it needs two buffered
  responses ahead of the stream rather than one: filed as #211 and fixed by
  parking the sender until the tail is empty, on a connection-lifetime event
  fired from every site that clears `out_in_flight`. Evidence: `h1/052`.

  A third writer stays open. The plaintext `sendFile()` body is submitted as
  `ZEND_ASYNC_IO_SENDFILE` straight at the io (`http1_sendfile.c:661`) while
  its own head goes through the queue, so nothing but `TCP_CORK` orders the
  two. Fifteen runs across nine shapes — first body 2 B to 4 MiB, reader delay
  0 to 400 ms — all came back in order, the file hop through the thread pool
  being slower than the drain in each. Unproven rather than filed.
- [x] **#179 — one serialized outbound path per HTTP/1 connection.** Closed by
  splitting the two callers rather than by moving the whole path off the await,
  which is what `dev/BENCHMARKS.md` refused to support. `write()` keeps the
  awaited sender and with it the return value that sets `stream_dead`, the
  bracketed deadline and the copy-free frame; `tryWrite()` queues on the
  connection's tail and refuses once the depth reaches the high-water mark.

  The depth counts the write in flight as well as the tail: without it the
  first write reads as zero and the refusal arrives one write late. Two holes
  the awaited path had been covering came with it — a queued write learns
  nothing from libuv, whose completion carries no status, so the read side now
  latches `write_failed` and the terminator refuses to seal a body that failed;
  and queued bytes carry no await to time out, so the connection's write
  deadline is armed at submit and stopped when the tail goes idle.

  Evidence: `h1/053`, restored from the commit that deleted it in #177 — 42
  chunks accepted, the 43rd refused, the handler still running. `h1/032` now
  answers a refusal with `awaitWritable()`: the twins never suspend, and a loop
  that only offers never gives the reactor the turn in which the peer's RST
  arrives, so its 499 used to rest on the very park this step removes.

## Landed elsewhere

- **php-async #260 / #261.** Every debug CI job broke on 2026-08-20 with
  `op_array_emalloc_copy_array: Assertion 'zend_hash_num_elements(src) ==
  src->nNumUsed'`. The helper rebuilds two tables and asserted an invariant that
  holds for one: static variables need every bucket, because BIND_LEXICAL reaches a
  captured variable by a byte offset into arData, while a literal array may hold a
  hole — `[0 => 'a', 2 => 'b']` compiles to a packed hash whose slot 1 is UNDEF.
  The assert moved to the caller whose offsets depend on it. Server commit `5392ff7`
  was green at 09:11 UTC and the same commit failed at 12:50 with nothing changed on
  its side, which is what identified the cause.

## The cmocka suite rots unnoticed (#189)

Nothing built these targets in CI, so production signatures moved and the tests kept
compiling against the old ones. Two were repaired on 2026-08-19 (`ResponseWire`: the
`bool complete` argument dropped in 6fbe731 on 2026-07-07; `TLSSession`: the ring size
the backpressure case measures is the CT-in half, `TLS_BIO_RING_SIZE_SMALL`, not the
CT-out one). `ctest` ran 10 of 16 on 2026-08-20 and runs 16 of 16 now.

- [x] **Five targets do not link.** Closed by linking the real `src/grpc/grpc.c` and
  `src/log/trace_context.c` — both leaf TUs, so the code under test answers — and by
  stubbing only what no case reaches: the request-finalize tail for the parser targets,
  three symbols for the session target and twenty-one for the strategy one. Every new
  stub aborts instead of answering, which is what keeps a target that starts driving
  such a path from passing on an invented result. Evidence: 8 + 24 + 3 cases pass with
  every stub still aborting.
- [x] **`test_http3_packet` segfaults** in `test_account_send_error_buckets`. It was the
  test: its last line called `http3_packet_account_send_error(NULL, EAGAIN)` under the
  comment "NULL-safe, no crash", while the function opens with
  `ZEND_ASSERT(st != NULL)`. With asserts compiled out the null went through. The line
  is gone; the contract stays where it was written.
- [x] **Build the unit suite in CI.** In the embedded-fuzz job, the only one that builds
  a libphp for the suite to link against.

## A phpt client reads a framed body with one fread (#233)

- [x] **Read until the declared count is in hand.** `fread($fp, $len)` on a network
  stream returns as soon as one segment is available, so a body the server sent as
  two writes read back short: `h1/037` compared five bytes against nine on
  MACOS_ARM64_RELEASE_ZTS in run 32590836568, and `h1/038`, `h1/042` and `h1/048`
  carried the same race unfired. `tests/phpt/server/_read_exact.inc` reads until the
  count is met or the peer closes, and all four clients call it. The server's framing
  was right throughout — `content-length: 9` and nine bytes on the wire — which is
  why no server code changed. Evidence: a probe that puts 30 ms between the two
  writes reads 5 bytes of 9 through one `fread` and 9 through the helper, three runs
  each; 473 phpt, 448 passed, 0 failed, 24 skipped, 1 warn (`h2/060`, the retry-pass
  that predates this).

## What the request path was losing (#236, #237, #238, #239, #240)

Five defects found in one pass, four of them shapes where the framing called the
response whole and the peer had less than the handler wrote. They are one section
because they were found and land together, not because they share a cause.

- [x] **#236 — an HTTP/2 request ran the HTTP/1 handler.** `conn->handler` was
  picked in the accept callback, before a byte had been read and so before the
  protocol was known: HTTP1 first, HTTP2 only where no HTTP1 handler was
  registered. `addHttp2Handler` therefore answered nothing on any server that
  also called `addHttpHandler` — which `docs/USAGE.md:116` names as its main use.
  The pick happens at both detection sites now, through
  `http_connection_bind_protocol_handler`, and `http_protocol_pick_handler` takes
  the protocol for the worker pool and HTTP/3. The general registration stays the
  fallback for every protocol. Evidence: `core/068` fails against `main` on the
  h2c line alone.
- [x] **#237 — a buffered HTTP/2 response carrying trailers lost its body.**
  nghttp2 takes trailers only from inside the data provider, at true EOF, with
  `NO_END_STREAM` on the last DATA slice; the buffered path queued them straight
  after `submit_response`, and the DATA that had not left yet was displaced.
  Three shapes were wrong: a 7-byte body delivered 0, a gzipped 4096-byte body
  delivered 0 decoded, and an empty body lost the trailers instead, because with
  no DATA frame the HEADERS carries END_STREAM. Such a response takes a
  zero-length DATA frame to hang them off now. Evidence: `h2/063` fails against
  `main` on all three lines; `h2/003` gained the body assertion whose absence let
  this pass for as long as it existed.
- [x] **#238 — three exits from `h1_stream_mark_ended` left the framing lost and
  unmarked.** The terminator's write answer was discarded — the only
  `(void)http_connection_send` in the tree — and the awaited write path latches
  nothing on the connection, so the next reader saw a healthy one and the request
  pipelined behind the unfinished chunk was answered into it. The guard at the top
  and the header-commit failure had the same hole: both returned without a
  terminator and without setting `stream_dead`, which is what
  `http_request_finalize` reads as "write nothing more". All three make the two
  marks `h1_stream_abort` makes now. Evidence: `h1/055` cancels the handler while
  `end()` is parked inside the terminator write, `h1/056` resets the connection
  with `SO_LINGER 0`; both fail against `main`. The header-commit branch is
  carried without a test — no seam makes a header write fail.
- [x] **#239 — `awaitBody()` answered none under a non-nullable return type.**
  Unreachable on today's engine, where `async_waker_new` raises `E_CORE_ERROR`
  before it can return NULL, but the macro dispatches through a function pointer,
  so the call site answers the contract rather than the implementation. It throws
  now: without a waker the body was never waited for, and `$this` would say it
  was.
- [~] **#240 — shared-fd accept has no arbitration.** Not answered here. Where the
  kernel has no load-balanced `SO_REUSEPORT` the workers share one descriptor and
  nothing arbitrates between their reactors, so a machine short of CPU lets one
  drain the accept queue: measured on Linux under
  `TRUE_ASYNC_SERVER_SHARED_LISTEN_FD=1` pinned to one core, `[1,0,8]` samples
  across three workers once in six runs, against a spread every time with
  SO_REUSEPORT. Whether the server should promise better is a design question with
  a cost on the accept path of every connection, and it stays in the issue. What
  is fixed is the test that had been silenced for it: `telemetry/011` asserted
  `PHP_OS_FAMILY === 'Darwin' || $reporting > 1`, which is nothing on the one
  platform where it went red. The assertion is gone rather than carved out — the
  same non-promise is already recorded in `websocket/042:116` — and the test went
  from 1 failure in 20 under those conditions to 0.

## A request the HTTP/3 peer abandons (#242)

Found by driving an HTTP/3 client that stops reading — the one shape no test in
the suite had, since every H3 test reads its response to the end.

- [x] **#242 — the in-flight bracket is closed and the stream released once.**
  Both halves came from the dispose reaching the server through `s->conn`, which
  connection teardown NULLs while the handler coroutine is still running. The
  dispatch raised `active_requests` unconditionally and the dispose lowered it
  under `if (c != NULL)`, so an abandoned request left the gauge up for good —
  and that gauge is what `http_server_should_shed_request` reads, a predicate
  only the HTTP/1 parser and the HTTP/2 session consult, so N abandoned HTTP/3
  requests subtracted N from the admission budget and the server eventually
  answered 503 to protocols that had done nothing. The same reach dropped the
  request from telemetry. Counters and log sink are taken when the stream is
  created now — at creation rather than at dispatch, because the static path
  never dispatches a handler, which the first version of the change broke and
  `h3/054` caught.

  The second half: `http3_stream_release` dropped `request_zv` and
  `response_zv` without `ZVAL_UNDEF` while `h3_dispose_tail` guards its own
  release with `Z_ISUNDEF` — a guard testing a sentinel the other site never
  set. A stream the peer cancelled with STOP_SENDING was therefore released
  twice: on a debug build the process aborts in `zend_objects_store_del`, on a
  release build it writes into a freed object bucket. Reproduced on `62c33da`
  with no local change.

  Evidence: `h3/063` fails against `main` twice over — `active=1 total=0` where
  it expects `active=0 total=1`, then the crash backtrace in place of the rest —
  and passes here, 10 runs of 10. `tests/h3client/h3probe.py` gained two modes
  behind env vars, `H3PROBE_ABANDON_MS` (drop the connection) and
  `H3PROBE_STOP_MS` (STOP_SENDING, connection kept), which is the pair that
  tells the two states apart. 478 phpt, 454 passed, 0 failed, 0 warned;
  `ctest` 16 of 16.

- [x] **#244, #245 — a handler killed by a bailout, and the slab slot it strands.**
  The first lead of the three, re-measured with a probe of my own and wider than
  it was written: HTTP/2 answers the half-built body too, not only HTTP/3 and the
  pool, and a bailed-out gRPC call reports `grpc-status: 0`. Every transport now
  goes through one predicate, `http_response_reset_after_bailout` — the block
  HTTP/1 already carried.

  The lead's "neither counted nor freed" split in two. Not counted is policy, not
  a defect: `http_request_finalize` skips a bailed-out request on HTTP/1 as well,
  because post-bailout EG cannot sustain the telemetry call. Not freed is #245
  and is not about bailouts at all — any handler that keeps the `HttpRequest`
  wrapper past `stop()` strands its slab slot, and the listener freed the slab
  under it. The pool is allocated on its own now and the last slot out frees it.

  Two things the first round of this work got wrong, both caught by running the
  reactor pool rather than reading it. The pool test carried `setWorkers(2)` and
  no `--ENV--`, which is not the reactor pool at all — every other `h3/*-pool-*`
  test sets `TRUE_ASYNC_SERVER_REACTOR_POOL=1`, and without it the test measured
  the direct path twice and timed out in CI. With the env on, the deferred slab
  free landed after the allocating thread's request heap was gone, so ZMM
  reported the pool as a leak and the heap as corrupt; the slab is persistent
  memory now.

  Evidence: `core/069`, `grpc/019`, `h3/064`, `h3/065`, `h3/066`, all five failing
  against `main`. 483 phpt, 459 passed, 0 failed, 0 warned; `ctest` 16 of 16.

- [x] **#247 — the direct HTTP/3 submit path disagreed with every other path about
  what reaches the wire.** The last two leads of the hunt, both measured with a
  probe of my own before a line of code, and both on the path
  `http3_stream_submit_response` and `h3_stream_mark_ended` own.

  **Trailers are dropped when the handler calls `end()` itself.** `end()` reaches
  `h3_stream_mark_ended`, which sets `streaming_ended` and drains, and the data
  reader can hit EOF inside that call — before the dispose runs
  `http3_stream_capture_trailers`. The gRPC ops already have the right shape:
  `h3_stream_finish_streaming` captures first, then ends. Measured with the
  aioquic client of `grpc/_h3grpc_client.py`, one trailer set, four handler
  shapes: `end()` after the write loses it in both orderings (trailer before the
  write and after it), while a stream the dispose finishes, a buffered response
  and every shape under the reactor pool keep it. HTTP/2 keeps all four. HTTP/1
  emits no trailers at all by design (`src/http_response.c:768`).

  **`H3_RESPONSE_HEADER_MAX` truncates and takes the Content-Length with it.**
  300 response headers plus a 512-byte body: the direct path delivers 254 of them
  and no `content-length`, while the body arrives in full — the framing the server
  computed is dropped without a word. The reactor pool delivers all 300 and the
  `content-length`; HTTP/2 delivers all 300 and the `content-length`. The cap is a
  `goto headers_done` out of the flatten loop with nothing told to the caller,
  and the header order decides which fields survive.

  The cap is removed rather than mirrored onto the other paths, and Edmond
  settled that: it arrived with the initial import behind no issue, its comment
  named the threat as "a server-side accident" rather than a peer — unlike the
  inbound caps in `http3_internal.h`, which name a malformed one — and it bounded
  10 KB of `nghttp3_nv` (40 bytes an entry, measured) copied out of a `HashTable`
  already holding the same strings at several times the cost. HTTP/3's real limit
  is the peer's `SETTINGS_MAX_FIELD_SECTION_SIZE`, a negotiated byte count the
  server does not read. A byte-denominated limit driven by that setting is a
  separate question, not opened.

  The trailer mechanism was proved rather than inferred: capturing in
  `h3_stream_mark_ended` before the latch turned all four shapes to `trailer=1`,
  and the experiment was reverted before the fix was written properly.

  Evidence: `h3/067`, `h3/068`, both failing against `main`. 485 phpt, 461 passed,
  0 failed, 0 warned; `ctest` 16 of 16.

- [x] **`h2/060` flakes because php-async builds a PHP object after the object
  store is gone.** The test's own assertions pass every time; the process
  segfaults afterwards, which `run-tests` prints as `Termsig=0` under correct
  output. Two failures in 17 full `-j4` sweeps, four in 30 standalone runs.

  Cause, from a backtrace of mine rather than a reading: `zend_deactivate` runs
  `shutdown_executor()` and only then `ZEND_ASYNC_ENGINE_SHUTDOWN()` — php-src
  states the contract at that line, "All objects are destroyed — safe to shut
  down the reactor", and `zend_shutdown_executor_values` has already set
  `EG(active) = 0`, "No PHP callback functions should be called after this
  point". `libuv_reactor_shutdown` then turns the loop to finish cancelling
  work, a connection this server closed earlier finishes closing there, and
  libuv flushes its queued write with `UV_ECANCELED`. `io_pipe_writev_cb`
  branches on `status == 0` alone and sends everything else to
  `async_new_exception` → `object_init_ex` → `zend_objects_store_put`, writing
  into an `object_buckets` that is NULL.

  The handle is ours (`stream->type == UV_TCP`,
  `free_cb == http_send_batched_writev_completion_cb`, `uv_flags == 0`, so no
  awaiter), but no frame of this repository is on the stack and the contract
  broken is php-async's. The fix belongs there and Edmond agreed to it: a check
  in `async_new_exception`, plus four sites that use its result without a NULL
  check — `libuv_reactor.c:2204`, `:2870`, `:3126`, `:6286`. The audit of all 27
  call sites is done; the other 23 carry NULL.

  No `.phpt` in php-async can reproduce it: the queue of writes on a stream
  handle exists only through `ZEND_ASYNC_IO_WRITEV_EX`, whose one consumer is
  this server, out of that tree. Measured, not reasoned — a temporary hook in
  `plain_wrapper.c` that leaves a write unawaited fires and leaks the request but
  exits 0, because a file write is waited out by the drain rather than cancelled;
  and `plain_wrapper.c:553` is the only caller of the write API in all of php-src,
  sockets not using it at all. So php-async gets a test-only entry point behind a
  build flag, agreed with Edmond, and the deterministic test rides on that.

  Done in php-async (issue #264, PR 265): `async_new_exception` answers NULL
  while `EG(active)` is 0, and the four call sites take that NULL. The entry
  point is a write queued at reactor shutdown when `ASYNC_FUZZ_CANCELLED_WRITE=1`
  is set, compiled in only with `--enable-async-fuzz`, and
  `tests/cleanup/005-write_cancelled_at_reactor_shutdown.phpt` rides on it:
  without the guard it prints its expected output and segfaults, with it 5 of 5
  runs pass. Against a PHP carrying the change, `h2/060` failed 0 of 30
  standalone runs where it had failed 4, and this suite ran 369 tests to 349
  passed, 20 skipped, 0 failed on that binary. The change reaches this
  repository only through a php-src build that carries it; `/usr/local/bin/php`,
  which the suite runs against by default, is older.

## HTTP/3 slot release under the reactor pool (#261)

- [x] **The route is stamped on the stream, not read through the connection.**
  `http3_stream_release_via_request` asked `s->conn` for the listener, and
  teardown NULLs `s->conn` while a worker still holds the request — so exactly
  the abandoned streams took the single-thread branch and called
  `http3_stream_pool_free` on the worker thread. Measured with a print in that
  branch, pool on and two workers, the abandon shape of `h3/063`:
  `conn=(nil) rctx=(nil) owned=1 refcount=1`. `http3_stream_release` twenty lines
  below already reads the stable `reactor_owned` flag and carries the comment
  explaining why `s->conn` cannot be read there.

  The stream now carries `req_reactor_id` and `req_reactor_pool`, taken at
  creation next to `req_counters` for the same reason. A closed pool stays local:
  its listener is gone, no reactor takes work for it, and both posts refuse
  there — `h3/065` timed out until that carve-out went in, which is how the
  shutdown path announced itself.

  The rule is tested rather than the race, the way #224 was: the route is
  `http3_stream_slot_goes_to_reactor` in `include/http3/http3_stream_pool.h`, and
  `HTTP3SlotRelease` puts the four shapes through it — an abandoned request with
  a NULL connection, a live one, a closed pool, and single-thread mode. Putting
  the pre-#261 rule back turns exactly the first of them red.

  Evidence: the print reads `owned=1 closed=0` and takes the reactor route after
  the change; `ctest` 17 of 17, 353 of 373 phpt (20 skipped, 0 failed). What no
  test covers is the free itself — it is a race rather than a deterministic
  fault, and on the shapes a phpt can build the skipped cleanup has nothing to
  release.

## Coverage through the seams the rules already have

- [x] **The framing rule is tested as a rule.** Asked for by Edmond on
  2026-08-24, and the pattern is #224's: a decision buried in a function that
  needs a live object is moved to a named function over its inputs, and the unit
  suite holds it to a table. `http_response_length_action_for` takes the six
  answers the rule reads — status, streaming, HEAD, `head_streamed`,
  `length_stated`, whether the table holds a count, and the declared length —
  and `http_response_length_action` fills them from the response. Both it and
  the H2/H3 header filter moved to `src/http_response_framing.c`, which holds
  nothing else, so the unit target links it without the response object behind
  it.

  `ResponseFraming` covers eleven cases: the four statuses that answer before
  the mode is read, a 205 outranking a stream, a measured buffer, a count the
  server stated, a declared and an undeclared stream, the two HEAD shapes, and
  the filter — hop-by-hop names, the content-length gate, and case folding.
  Ignoring `head_streamed` in the rule turns exactly
  `test_head_that_streamed_states_only_what_it_holds` red.

  Evidence: `ctest` 18 of 18, 353 of 373 phpt (20 skipped, 0 failed).

- [ ] **What a seam cannot reach, and why.** Three debts named in the CHANGELOG
  stay uncovered, and none of them is a rule: the execute frame a firewall
  restores after a bailout is engine state a unit target cannot bring up; the
  credit wait's bound is `caller ?: deadline`, while the defect was a call site
  ignoring its argument, which such a test would not have caught; and the
  header-commit branch of `h1_stream_abort` needs a seam that makes a header
  write fail, which no shape reachable from PHP produces.

## Release (asked for by Edmond, 2026-08-24)

To run once the defects above are closed. The algorithm is `~/releases/RELEASING.md`;
these are the steps before and around it.

- [x] **php-src: bring the fork forward.** `master` already matched upstream;
  `true-async` was 204 commits behind it. The merge is `cf960704dfb`, pushed to
  both `true-async` and `true-async-stable`. Two files conflicted, both fiber
  tests, where upstream broadened the catch to `Throwable` and printed the
  exception class while the fork keeps its own expectation — the force-closed
  suspend escapes as a fatal here, and the unfinished-fiber test prints `done`
  ahead of the finally block. Both take upstream's body with the fork's SKIPIF
  and order.

  It needed php-async beside it: upstream passes `$this` as a `zend_object*`
  now, and `ext/async` carried a copy of the closure layout that still said
  zval, so 37 thread tests segfaulted in the worker (true-async/php-async#268).
  Evidence: `Zend/tests/fibers` + `ext/async/tests` read 1286 of 1289 on the
  merged tree, the same three failures it had before the merge, against 1249 of
  1289 with the merge alone.
- [x] **Tag php-src** — `php-8.6.0-trueasync-0.9.7` on `cf960704dfb`, with notes
  naming the ABI addition and the two fiber-test resolutions.
- [~] **php-async: bump the version inside the extension, then tag** `v0.9.5`
  from `main`. The macro said `0.9.3` while the tags had reached `v0.9.4`, so
  `phpinfo()` reported a release behind; PR 269 carries it and the tag follows
  its CI.
- [x] **Server: build against the new PHP, run the suite, bump the version,
  tag** — `v0.13.0` on `8c80d42`. A minor: the release carries the body-API
  renames. Verified against the candidate PHP (php-src `true-async` merged with
  `master`, php-async #268, installed as `/home/edmond/php-release-26`): 353 of
  373 phpt, 0 failed, built against it.
- [x] **`~/releases`: point `build-config.json` at the new tags, commit, tag**
  `v0.9.7` — done for the 0.9.7 round.
- [ ] **Every tag carries release notes of its own.** Asked for by Edmond on
  2026-08-24: each repository gets a written description of what changed, not a
  bare tag. `~/releases/.release-notes-*.md` are the earlier ones.

  Open: the product version for this round is Edmond's to name.

## The document-root mount (#259)

- [x] **A `StaticHandler` mounts at `/`.** Found while answering
  YanGusik/laravel-spawn#52, where a user's files under Laravel's `public/`
  reach the catch-all route instead of the disk. That report is the adapter's —
  it ships `'static_handlers' => []` and mounts nothing — but the mount it needs
  is the document root, and the constructor refused it: `len < 2` in
  `validate_url_prefix` threw the message about brackets at an argument that
  brackets. The bound is gone; an empty string is still refused, and
  `http_static_path_resolve` needed nothing, because a one-character prefix
  leaves the whole path as the relative one.

  Evidence: `static/022` reads a file off the disk at `/assets/app.svg`, gets
  `handler:/api/users` for a path the mount does not hold, and `handler:/index.php`
  for one `hide('*.php')` covers — so a document root does not answer with PHP
  source. It fails against `c748c4b` at the constructor. `static/016` recorded
  the old bound as `ctor:prefix-too-short` and now records the new answer.
  353 of 373 phpt (20 skipped, 0 failed), `ctest` 16 of 16.

## Which worker accepts a connection (#240)

- [x] **The server promises nothing, and the shared-fd path is now tested.**
  Settled by Edmond on 2026-08-24: fairness between workers is not a promise the
  server makes, so no arbitration is added. That closes the second question with
  it — a mutex or a round-robin token sits on the accept path of every
  connection, and nothing is paying for a guarantee nobody gives.

  What the answer leaves is coverage. Where the kernel has no load-balanced
  `SO_REUSEPORT` — macOS, the other BSDs, Solaris — the parent binds one socket
  and every worker accepts off a dup of it, and no runner exercised that model:
  `TRUE_ASYNC_SERVER_SHARED_LISTEN_FD=1` existed for it and nothing set it. The
  Linux debug leg now runs the whole suite a second time with it. Measured
  locally first: 352 of 372 (20 skipped, 0 failed) on the shared fd, 65 s against
  68 s for the ordinary pass, so the model costs the leg about a minute.

  `setWorkers()` says the non-promise where a reader meets it, next to the
  sentence about the kernel balancing accept.

## A response kept past its request (#256)

- [x] **The transport pair is cleared with the context it points at.** Found by
  auditing the response lifecycle after #235. `stream_ops` and `stream_ctx` are
  installed at dispatch and outlive the per-request context whenever PHP code
  keeps the `$response` — a global, a queue, a cache. The API guard does not
  cover it: `isWritable` and `response_check_stream_usable` refuse on `closed`,
  on a pending `sendFile()` and on a NULL `stream_ops`, and a buffered response
  is none of the three when its request ends.

  Measured on HTTP/1 with a debug build: a handler stashes one `$response`, 200
  requests recycle the freed `http1_request_ctx_t`, and the next `isWritable()`
  on the kept object segfaults in `h1_stream_is_alive`
  (`src/http1/http1_stream.c:598`, reading `ctx->conn`). The reactor pool was
  already clearing the pair one line before its own dtor
  (`src/core/worker_dispatch.c:777`); the three transports do the same now.

  What is not measured: the HTTP/2 and HTTP/3 halves. Both hand their slots back
  to a pool, so there a stale pair points at a live foreign stream rather than at
  free memory, which is worse than a crash — but the shape needs a slot to be
  reused by another request while the kept response is touched, and no probe of
  mine has produced it.

  Evidence: `core/070` passes on the fix and dies with a signal against
  `8bff3f3`; 352 of 372 phpt (20 skipped, 0 failed), `ctest` 16 of 16.

## A half-closed peer lost the rest of its response (#249)

- [x] **The read side stopped standing in for the write side.** Found while
  building the reproduction #225 still owed: a peer that calls
  `shutdown(SHUT_WR)` and goes on reading was answered with 110 bytes and no
  chunked terminator where 4 MiB were owed, and over HTTP/2 with 65536 to 393216
  DATA bytes of 4194304 and no END_STREAM. Every terminating read latched
  `conn->write_failed`, and a clean EOF is what a half-close produces.

  The one-line experiment named the cause: `conn->write_failed = err` and the
  whole body arrived. It is not the whole fix, and `h1/032` said so — with that
  alone the handler ran all 100000 iterations of 4 KiB against a peer that had
  sent an RST and never saw the 499 its contract promises. The kernel hands
  `so_error` to whichever syscall asks first, and with a saturated queue that is
  the write: on bare sockets after an RST, `write -> ECONNRESET`,
  `write -> EPIPE`, `read -> b''`.

  So the write reports for itself. `ZEND_ASYNC_IO_WRITE_FAILED` is set by the
  reactor on a failed write (true-async/php-src#27, ABI 0.26.0;
  true-async/php-async#266) and read by every write completion here, which also
  releases the outbound tail rather than chaining another refused write. The
  read side latches on a read error alone, under the version guard the file
  already uses.

  Evidence: `h1/057` and `h2/064`, both failing against `main`; `h1/032` holds
  the other half. 351 of 371 phpt (20 skipped, 0 failed), `ctest` 16 of 16.

  Two fixes recorded in the CHANGELOG were carried without a test. The
  reproduction for #225 is what turned into this step, and the step closed the
  debt on its way past — see below.

## The two CHANGELOG debts, #224 and #225

- [x] **#225 was already covered, and #224 now is.** Both entries said "carried
  without a test", and neither claim survived being checked. `h2/064` builds the
  peer #225 asked for: a half-close with a writev in flight defers the destroy,
  and the re-drive in `http_send_batched_finish` is what gets the last frames
  out. Put that completion tail back the way it was before #223 and the test
  fails 10 runs of 10 — all 1048576 body bytes arrive and END_STREAM does not,
  which is the entry's own description of the defect. It passes 10 of 10 with
  the tail in place, so the entry now cites `h2/064` instead of an excuse.

  #224 had no test and could not get one in the shape the entry imagined. The
  parked slice it needs appears only in DRAIN, and DRAIN is picked when
  `large_streams_pending == 0` — but a body over `H2_TLS_HYBRID_LARGE_THRESHOLD`
  (2048, strict `>`) raises that counter itself, so no single large response
  reaches the window. It takes a dozen streams of exactly 2048 bytes behind a
  zero initial window, then one TCP write carrying the WINDOW_UPDATE batch plus
  a request for a large body — at frame level over TLS, which the suite cannot
  speak: `_h2_client.inc:60` opens `tcp://` and nothing else here talks h2 over
  TLS frame by frame. Even then a reactor tick between the microtask and the
  handler coroutine can flush the parked tail and close the window, so the phpt
  would be probabilistic.

  So the invariant is tested instead of the wire. The mode choice moved out of
  `h2_session_emit_ex` into `h2_tls_emit_use_drain`, declared in
  `include/http2/http2_session.h` and compiled with or without OpenSSL so the
  unit suite links it. Four cases in `HTTP2Strategy` cover the selector, and
  removing the pin turns exactly one of them red:
  `test_emit_selector_parked_slice_outranks_the_mode`. What this does not cover
  is the truncated frame on the wire, and the CHANGELOG says so.

  Evidence: `ctest` 16 of 16, 351 of 371 phpt (20 skipped, 0 failed).

## awaitWritable() on HTTP/3 (#265)

- [x] **The wait HTTP/3 did not have.** `h3_stream_ops` declared no
  `wait_writable`, so `awaitWritable()` took the branch meant for a transport
  that cannot wait and answered false whenever the queue was full. The stream
  already owned every part of the wait: `s->write_event`, fired by
  `extend_max_stream_data_cb` and by the data reader, and the park loop inside
  `h3_stream_append_chunk`. That loop is now `h3_stream_wait_for_room`, called
  by both `append_chunk` and the new `h3_stream_wait_writable`, and its deadline
  is the shorter of the caller's `timeout_ms` and the connection's write
  timeout — the rule `h2_wait_for_drain_event` already states.

  The one behaviour the extraction had to keep: outside a coroutine the flush is
  best-effort, not a dead stream. `append_chunk` asks `h3_can_suspend()` before
  entering the wait, so the refusal the wait gives there does not reach the
  caller as `HTTP_STREAM_APPEND_STREAM_DEAD`.

  Evidence: `h3/069` reads `room=0` against `main` and `room=1` with the fix —
  the aioquic probe caps its window at 16384, so the second 256 KiB chunk sits
  on the queue behind a blocked stream while the wait is asked for. 354 of 374
  phpt (20 skipped, 0 failed), `ctest` 18 of 18.

  Not covered: that the caller's bound is honoured when the window never opens.
  It needs a peer that stalls forever, which the aioquic probe does not offer;
  the bound itself is the same expression H2 is tested on.

## The label an aborted response carries (#267)

- [x] **Two questions, two fields.** `http_response_finish_stream` set `aborted`
  inside the branch the transport's `abort` op answered true to, and set
  `closed` outside it. A transport answers false when nothing of the response
  has reached the wire, so `sseStart()` followed by `abort()` left the pair
  `aborted = false, closed = true` — the state a clean `end()` leaves. Six
  refusals read `aborted` and all six then named the wrong call; the second
  `abort()`, documented as a no-op, threw instead, in the `catch` block where it
  replaces the handler's own diagnosis.

  `aborted` is now which call finished the response, set from `failed` outside
  the branch, and `wire_failed` is what the peer saw. `http_response_is_aborted`
  reads the second, so the header's stated contract — the body was disowned
  mid-flight — holds without a word changed.

  The alternative was to keep one field and set it in both branches. It was
  rejected because `responses_aborted_total` and `error.type` would then count a
  request the peer received whole; `core/066` was extended so that choice cannot
  be taken silently — with the two questions merged back it reads
  `responses_aborted_total=2` and `error=response_aborted` for a completed
  request.

  Evidence: `core/063` covers the third state a started stream can be in
  (nothing on the wire) and fails against `main` on all three of its new lines.
  354 of 374 phpt (20 skipped, 0 failed), `ctest` 18 of 18.

## Four defects found by review on 2026-08-24, three of them open

Verified this day, each by a run or by a reading that names the code. None is in
a release yet except the first two, which are fixed.

- [x] **`awaitWritable()` refused instead of waiting on HTTP/3 (#265).** Fixed;
  the section above carries the evidence.
- [x] **An aborted response with nothing on the wire was labelled cleanly ended
  (#267).** Fixed; the section above carries the evidence.
- [ ] **Compression is not wired into the worker path.** Settled by Edmond on
  2026-08-24: a defect, not a feature awaiting a port. With the reactor pool on,
  a response is not compressed and a `Content-Encoding` request body reaches the
  handler as it came. `http_compression_attach` runs in all three transports —
  `src/core/http_connection.c:2654`, `src/http2/http2_strategy.c:289`,
  `src/http3/http3_dispatch.c:668` — always beside the response object it
  belongs to, and `http_compression_decode_request_body` runs beside each
  handler call. `src/core/worker_dispatch.c:860-869` builds its own request and
  response on the worker thread and calls neither, so the H3 dispatch path that
  would have attached is never reached: the request is marshalled and the
  response is a different object. `json_encode_flags` is missing there for the
  same reason, and the protocol version is hard-coded `"3.0"` two lines above a
  `ctx->protocol` that already distinguishes HTTP/1 and HTTP/2 — harmless while
  the pool is H3-only, which #106 is about ending.

  **The trap the fix has to clear first.** `http_server_get_config(server)`
  reaches a `HashTable` — `compression_mime_types`, read by `decide()` at
  `src/compression/http_compression_response.c:265` — allocated in the thread
  that built the config. A worker thread must take the frozen snapshot instead:
  `http_server_shared_config_t` (`src/http_server_config.c:50`) deep-copies that
  whitelist into persistent `zend_string`s for exactly this crossing, and each
  PHP thread LOADs its own `http_server_config_t` from it. Which of the two the
  worker actually holds is the first thing to establish; the attach call is
  correct only over the second.

- [ ] **A full reactor mailbox drops a buffered response and the record says it
  was delivered.** `reactor_pool_post_exec` answers false when the mailbox is at
  capacity (default 1024, floor 64), and `src/http_server_class.c:3053-3073`
  defers a STREAM_* wire to the retry FIFO while discarding a `RESPONSE_WIRE_FULL`
  on the spot. The discard itself is a decision the code states. What is not
  stated is the silence: `src/core/worker_dispatch.c:269` guards two unrelated
  decisions with one condition, so a FULL wire is exempted from
  `worker_wire_dropped_total` along with `stream_failed`, and
  `http_request_telemetry` twenty lines later counts the status and emits the
  access line regardless. The client waits out its own deadline while the server
  records a 200 that never left the process.

  Observed under gdb by forcing `thread_cmd_mailbox_post` to answer false for
  `http3_reactor_apply_response`: `CLIENT_OUT=[h3client: timeout]`,
  `responses_2xx_total=1`, `worker_wire_dropped_total=0`. Natural overflow was
  not reachable — 200 concurrent buffered requests and 50 streams × 3000 chunks
  against a 64-deep mailbox all served cleanly, because the per-stream credit cap
  throttles the producer first. So the consequence is established and the
  frequency is not.

  The fix is at `worker_dispatch.c:269`: split the condition, keep FULL exempt
  from `stream_failed` and count every undelivered wire. Beside it the invariant
  worth stating: a request whose wire was not delivered is not a delivered
  response, which means the telemetry call at `:768` has to see the flag.

- [x] **SSE never counts the bytes it sends (#336).** Fixed at the call site:
  `sse_dispatch` adds the record to `written_length` before `append_chunk` and
  takes it back for a dead stream or a refused non-blocking offer. `core/080`
  logged 11 of 69 octets on HTTP/1 and HTTP/2 before, 69 after, 20 of 20; the
  single funnel suggested below is not built. HTTP/3 not run.

  `sse_dispatch`
  (`src/http_sse.c:257`) hands its record to `append_chunk` without going through
  `response_check_declared_length`, the only function that advances
  `written_length` — and `written_length` is what
  `http_response_get_sent_body_size` reports for a streaming response, so
  `http.response.body.size` reads 0. The other five `append_chunk` call sites all
  pass through it. Observed on HTTP/1 and HTTP/2: three `sseEvent()` put 105
  octets on the wire and logged 0; the same handler finishing with
  `end("data: bye\n\n")` logged 11, which is exactly the part that went through
  the ledger. HTTP/3 not observed.

  The counting fix goes at that call site, with the release-on-refusal symmetry
  `tryWrite()` uses, since `trySseEvent()` passes `nonblocking = true`. The wider
  answer is that `append_chunk` is callable directly at all: one funnel that both
  records and dispatches would make the table above collapse to one row.

- [ ] **A failed HTTP/3 submit leaves `chunk_queue` primed.**
  `h3_stream_append_chunk` initialises the queue before
  `http3_stream_submit_response` is known to have succeeded, and its failure tail
  releases the elements while leaving the array allocated. `chunk_queue != NULL`
  is what `h3_stream_abort` and `h3_stream_mark_ended` read as "the response is
  on the wire", so such a stream is either reset without the peer having seen a
  HEADERS frame or left with nothing sent at all. H2 corrected the same primer in
  #171 — `h2_stream_init_ring` publishes the ring only after the commit — and H3
  was not touched.

  Not reproduced: the failure needs `Z_ISUNDEF(s->response_zv)` or an nghttp3
  NOMEM, and no live path reaches either today. A debt against a refactor, not a
  fault in the field.

## Release 0.14.0 (2026-08-24)

- [x] **Server: bump the version and cut `v0.14.0`.** A minor rather than a
  patch: `awaitWritable()` answers where it used to refuse on HTTP/3, and the
  refusals a finished response gives name a different call. Both are visible to
  a handler.
- [ ] **`~/releases`: point the server at `v0.14.0`, commit, tag `v0.9.8`.** A
  patch on the product version — php-src and php-async are unchanged since
  `v0.9.7`, and only the server extension moves. Notes for both tags are written
  by hand; the release workflow does not read them, so `gh release edit
  --notes-file` follows the build.

## `hide()` was anchored, and a document root leaked sources (#270)

- [x] **A pattern naming no directory names a file, at any depth.** Found while
  answering YanGusik/laravel-spawn#52, where the adapter has to mount Laravel's
  `public/` at `/` and `hide('*.php')` is the whole of what keeps the front
  controller off the wire. `fnmatch(..., FNM_PATHNAME)` stops `*` at each
  separator, so the pattern covered `index.php` and served `admin/tools.php` as
  its own text. Probed before reading further: `/index.php` answered
  `handler:/index.php` and `/sub/deep.php` answered `<?php echo "NESTED
  SECRET";`.

  gitignore's rule is the one an operator already knows and the one that fails
  safe, so it is what the code follows: no separator in the pattern means it
  names a file, and the file is hidden wherever it sits; a separator means the
  pattern is anchored at the mount root and `*` stops at each separator, which
  is the only way to say `cache/*` and mean that one directory.

  The rule moved out of the mount into `http_static_hide_glob_matches`, over the
  two strings it actually reads, and `StaticHide` holds it to a table: putting
  the anchored rule back turns it red. `static/023` shows the same end to end.

  Evidence: 355 of 375 phpt (20 skipped, 0 failed), `ctest` 19 of 19.

- [x] **`test_static_decoders` has been switched off, and nothing said so (#354).** Its
  CMake guard tests for `src/static/http_static_mime.c` and
  `http_static_etag.c`; both were renamed to `src/http_mime.c` and
  `src/http_etag.c` with their headers, so the guard has been false ever since
  and the target is skipped with a `message(STATUS ...)` nobody reads. The file
  also calls `http_static_mime_lookup`, which no longer exists — the current
  calls are `http_mime_lookup_by_ext`, `http_etag_format_strong` and
  `http_etag_match_inm` — so reviving it is a rewrite of about thirty cases, not
  a path fix. Belongs to #189, and the new `StaticHide` target is registered
  without a guard for the same reason.

## Two room tests rested on an accept split the server does not promise

- [x] **The spread is proved now, not assumed.** `064-room-retry-delivered` and
  `066-room-retry-rejected` opened twelve subscribers across two workers and
  went on as though a remote worker certainly held one — 064 said so in a
  comment. The outbound queue they exercise is reached only through a
  cross-worker post, so a run where one worker took every accept sends locally,
  parks nothing, and the counters never move: `066` read `a=1 b=1` and
  `retry_rejected advanced: no` on `LINUX_X64_DEBUG_ZTS` in PR 269. Which worker
  accepts is exactly what #240 settled the server promises nothing about, so the
  premise was never a fact.

  `ws_open_spread_subscribers` opens connections in rounds and probes with a
  publish from the parent, which posts once per worker holding a subscriber:
  `ws_topic_posted` moving by the worker count is the observable that turns the
  hope into a fact. The probe's own messages are drained before it returns, and
  a spread that never happens is reported rather than asserted past.

  Evidence: asking for a spread across three workers on a two-worker server
  prints `no subscriber on a remote worker` and fails, so the guard is load
  bearing; both tests pass 10 runs of 10, and the whole phpt tree reads 471 of
  491 (20 skipped, 0 failed) on a build configured with
  `--enable-tas-test-hooks`.

## The Windows build linked nothing, and its own job could not tell (#271)

- [x] **`config.w32` had fallen behind `config.m4`.** The release pipeline's
  Windows job — the only place that compiles `config.w32` — stopped at 18
  unresolved externals on the `v0.9.8` build: `http_response_framing.c` was
  never in the list, so `http_response_length_action_for` and
  `http_response_header_allowed_h2h3` had no definition, and `src/room/` was
  absent entirely while `HttpServer::publish()`, `::send()`, `::trySend()`,
  `::room()`, `::subscriberCount()` and `::enableRooms()` are declared whatever
  the WebSocket setting is. Both are added. WebSocket stays out, as it was: it
  needs wslay, and its call sites are behind `HAVE_HTTP_SERVER_WEBSOCKET`.

- [~] **`WINDOWS_X64_ZTS_RELEASE` is green on an absent extension.** It is why
  the drift was invisible. The job checks out php-src, copies the extension in,
  runs `configure.bat --enable-true-async-server` and `nmake` — and the build
  log contains not one compile line for an extension source. The phpt step then
  reports `Number of tests : 491` with `0` executed and `Tests failed : 0`,
  because every test skips on a missing extension, so the job passes.

  Two guards are in: the test step asks `php.exe -n -m` for the module before it
  trusts anything the run says, and `assert_executed.php` refuses a run where
  nothing executed — checked against the failing job's own summary line, a real
  one-test run, an unparseable log and a missing file, which answer 1, 0, 2, 2.

  **The job will be red until the build is fixed, and that half is still open.**
  The fact is settled: 735 sources compile in that job and not one belongs to
  `ext/http_server` or `ext/async`, and neither name reaches the configure
  summary's enabled-extension table. So the cause is common to both extensions,
  which rules out anything specific to this one — the directory name among them.

  Ruled out from the log: discovery (`buildconf.js:find_config_w32` enumerates
  every `ext/*` holding a `config.w32`, and both were copied before
  `buildconf.bat --force` runs); an unknown flag (`confutils.js:413` errors on
  one, and configure did not); the ZTS refusal in `config.w32` (`THREAD_SAFE=1`);
  a failed `CHECK_LIB` (each calls `ERROR()`, which fails configure). Both
  `ARG_ENABLE` names match what is passed — `async` and `true-async-server`.

  What the log cannot answer is what configure was actually given and what it
  made of it, because `build_task.bat` runs under `@echo off`. It echoes the
  invocation now and asks `configure.bat --help` what it knows of the two flags,
  so the next run says. Settling it likely needs a Windows host: the SDK, the
  toolchain and `cscript` are not reachable from here.

  Asked of the people who own the recipe: true-async/php-async#270. The one
  difference between their job, which builds `ext/async`, and this copy, which
  builds neither extension, is `--enable-snapshot-build`. By
  `confutils.js:461-510` that flag rewrites only arguments the command line did
  not name, so an explicit `--enable-async` should stand without it — the
  observation and the source disagree, and that disagreement is the question.

  **A trap this cost an hour of CI:** a batch file edited from Linux must keep
  its CRLF. `cmd.exe` parses a parenthesised block by line ending, so an
  LF-only `build_task.bat` fails at its first `if … (` with
  `'/i' is not recognized`, which reads like a bad flag and is not.

  **Answered.** The flag was masking the fault, not carrying the build.
  `buildconf.js:find_config_w32(".")` scans every directory in the php-src root
  for a `config.w32`, so `async-src` and `http-server-src` register as modules
  beside the copies under `ext/` and every `ARG_ENABLE` lands twice. The first
  entry takes the command-line flag; the second is never named, so the loop that
  applies defaults assigns `PHP_ASYNC = "no"` right after. The snapshot pass
  rewrites exactly those unnamed `'no'` defaults to `yes,shared`, which is why
  the ext/async job built its extension and this copy built neither. Their own
  job hid a milder form of the same thing: the staging directory is named
  `async`, collides with `ext/async` by name, and the log reads
  `Skipping ext/async -- already have a module with that name` — it was building
  the root copy. Both recipes now delete the staging checkout before
  `buildconf.bat`; the flag stays out, and `PHP_HTTP3` keeps its `'no'` default.

## An aborted HTTP/2 stream lost the response it had written (#285)

- [x] **The reset dropped the frames the emit had not sent yet.** `h2/053` read
  `status=0` and an empty body on Windows, 3 runs of 3, while RST_STREAM(2)
  arrived and the next stream on the connection was served. `h2_stream_append_chunk`
  ends with `http2_session_emit`, which skips the send while a writev is in
  flight and leaves the frames to the write completion's re-drive; on Windows the
  initial SETTINGS writev is still in flight when the handler's first `write()`
  runs. `h2_stream_abort` then submitted RST_STREAM, and nghttp2 drops what it
  has queued for a stream it moves to the closing state — the HEADERS commit
  among it. The ring-exists check guarding the reset reads a non-NULL
  `chunk_queue` as "the HEADERS are on the wire", which a skipped emit makes
  false.

  The abort flushes through `http2_session_emit_now` before raising
  `streaming_ended` — with that flag the provider marks EOF on the last DATA,
  and an END_STREAM would call the truncated body whole — and its closing emit
  moves to `emit_now` as well, the pump documented for a caller that will not be
  there when the write completion re-drives.

  The feedback loop came before the reading: the test failed 3 of 3, and a copy
  with a 100 ms delay before the first `write()` passed 3 of 3, which named the
  race before any code was read. After the fix the test passes 5 of 5 and
  `tests/phpt/server` gains no failure.

  **The old expectation was one platform's timing.** `body=alpha` held because
  the first chunk had been framed and the second had not; on Windows it produced
  nothing at all. The contract is now the same everywhere: everything the handler
  wrote before the throw reaches the peer, RST_STREAM says the body stops there,
  no END_STREAM. The test asserts `body=alphabeta` and fails without the fix on
  Linux too.

## A refused upload never heard the refusal (#287)

- [x] **The read buffer filled after the parse error, and the read died of it.**
  `h1/005` read an empty reply on Windows while `parse_errors_413_total` counted
  the refusal. The failing read says what happened:
  `Async\InputOutputException: Pipe read error: no buffer space available`,
  `UV_ENOBUFS`, and that error latches `write_failed`
  (`src/core/http_connection.c:1284`), which takes the connection down before
  the 413 is delivered. `http_connection_handle_read_completion` returns early
  on `parse_error_handled` and consumes nothing, so `read_buffer_len` stays put
  and grows with every chunk the peer sends; `http_connection_alloc_cb` then
  hands the reactor a zero-length buffer, which is how this transport spells
  backpressure and how libuv spells a fatal read.

  Behind it waits the second effect, which the same fix removes: a socket closed
  with unread bytes in its receive buffer is reset rather than finished, and the
  reset discards what this side has written.

  A parse error now opens a lingering close — nginx's `lingering_close`,
  Apache's `ap_lingering_close`. Arriving bytes are dropped instead of buffered
  and the destroy defers: 500 ms of silence from the peer, refreshed by every
  chunk, and 5 s in total. Both reach the connection through the deadline tick
  already walking the connection list, so the change adds no timer and the tick
  period is the resolution — the wait is at least this long, not exactly.

  Three defects a review found before it landed, all fixed here. **A peer
  trickling one byte per window held the connection for good:** the idle
  refresh did not stop at the cap, and the cap was read only by a destroy that
  such a peer never triggers. **The cancel path did not wait at all:**
  `http_request_finalize` zeroes `deadline_ms` for a connection going to close,
  which cancelled the drain the moment the refusal was written — the case the
  test covers, passing on loopback timing rather than on the wait. **A drain
  outlived its server:** the sweep in `http_server_free` runs after the deadline
  tick is stopped, so a deferred destroy there was never finalised and the
  connection, its io, parser and strategy leaked; the sweep ends the drain
  itself.

  The loop came first: the test failed 3 of 4 runs, and a copy cut to exactly
  the bytes that trip the limit — nothing left inbound — passed, which named the
  condition before any code was read. After the fix `h1/005` passes 5 of 5 and
  `tests/phpt/server` loses two failures without gaining one.

- [x] **A full read buffer kills any connection, not only a refused one (#341).**
  Fixed: the read pauses on a full buffer and resumes at half; `h1/060`
  104 of 151 and 64 of 3001 answered before, all after; `h1/061` (413 and
  stop() while paused) and `websocket/074` (frames behind the upgrade). TLS
  reads through its own one-shot path, which the pause does not touch: 151 of
  151 and 3001 of 3001 through `openssl s_client` with this change; the old
  code was not run over TLS.
  `http_connection_alloc_cb` answers a full buffer with `base = NULL, len = 0`,
  and libuv turns that into `UV_ENOBUFS` on the read — a fatal error where the
  intent was backpressure. The drain above removes the parse-error case. Every
  other path that can fill `read_buffer` still ends this way, and none of them
  has a test.

- [x] **The `/dev/null` sweep is not worth doing; six files were (#295).** The
  literal appears 229 times across 108 files in `tests/` on `main`
  (`grep -ro '/dev/null' tests/`), and the question was whether to replace it
  wholesale for Windows. Measured instead: of the 214 tests the Windows suite
  skipped on 2026-08-25, **none** skipped because of `/dev/null`. Every
  `exec('curl --version 2>/dev/null')` in a SKIPIF sits under a `die` for
  Windows that fires first, and most of the remaining occurrences are in `.sh`
  files Windows never runs. What the literal did cost was one red test,
  `tls/016`, whose curl never ran; behind it hid a second Windows defect, the
  text-mode pipe that strips CR from `shell_exec` output and defeats a
  `\r\n\r\n` search. The measurement also named the POSIX-ism that does cost
  coverage, `command -v`: four `compression/` SKIPIFs, where `gunzip` compounds
  it by being a shell script in Git for Windows, and `core/068`, which asks for
  curl by a probe cmd.exe cannot run and then asks that curl for
  prior-knowledge HTTP/2 — the probe now asks `h2_skipif(['curl_h2'])` for what
  the test actually needs. On this build `core/068` is out of reach of either
  probe: it declares `--EXTENSIONS-- sockets`, which the build lacks, so the
  test skips before SKIPIF runs; the SKIPIF executed by hand answers
  `skip curl lacks HTTP/2 support`, which is the truth about a Schannel curl.

  Evidence, `tests/phpt/server` on Windows, 2026-08-25: 379 tests, from
  163 passed / 2 failed / 214 skipped to 167 passed / 1 warned / 1 failed /
  210 skipped. The warned one is a test that passed on retry under `-j4`, and
  which test that is moves between runs. The rest of the literals stay: each
  bites only when its test starts running on Windows, and it is cheaper to fix
  one then than to rewrite 228 sites now.

## The accept split, two tests further (#296, #297)

- [x] **`telemetry/012` and `websocket/065` rested on the same unproved premise
  as the two room tests.** The family was thought closed with 064 and 066; it was
  not. `telemetry/012` asserted `PHP_OS_FAMILY === 'Darwin' || $holders > 1` and
  failed twice running on `LINUX_X64_DEBUG_ZTS` with all six connections on one
  worker. The line could not simply go: it is the witness that the test's
  discriminating condition occurred at all, because with a single holder the sum
  across workers, the maximum and that worker's own field are one number, and
  the regression it exists to catch — the twenty-two counters back on the server
  object — would read green. Connections are opened in rounds now, the earlier
  ones held, until the per-worker column shows two holders; the gauge assertions
  compare against what is held rather than a constant, a run that never spreads
  prints `gauge_spread=none`, which no `--EXPECTF--` branch matches, and macOS,
  where a SO_REUSEPORT set hands every accept to one socket, runs one round and
  says `darwin-none`. `websocket/065` opened twelve subscribers in a plain loop
  where 064 and 066 already called `ws_open_spread_subscribers`; it takes the
  same call.

  Evidence: `LINUX_X64_DEBUG_ZTS` on #296 and on #297, both suite runs of the
  job green — 469 of 495 and 470 of 495, 0 failed — with `telemetry/012` and
  `065` executing rather than skipping.

  Left open: `errors_summed` in `telemetry/012` has never had a spread guard and
  still has none, so its summation is proved only on a run where the six bad
  requests did land on more than one worker. The file's header comment says so.

## The room retry tests on the shared listen fd (#329)

- [x] **`064`–`066` skip on the shared listen fd.** They need a subscriber on each
  of two workers, which the server does not promise (#240), and on the shared fd
  it measurably does not happen: `065`/`066` failed there three times in two runs
  of PR #328, and locally `065` read `no subscriber on a remote worker` on 3 of
  10, 5 of 20 and 2 of 20 runs against 0 of 16 on SO_REUSEPORT. The instrumented
  helper showed the failing runs at `[0,4]` after round 0 and `[0,32]` after
  round 7; in passing ones the first worker held 1 to 3. The retry queue they
  test (`room_hub_post_locked`) reads nothing about the listener and stays
  covered by the first CI pass. `tests/phpt/websocket/_ws_spread_skipif.inc`
  mirrors `core/076`'s condition. Evidence: with
  `TRUE_ASYNC_SERVER_SHARED_LISTEN_FD=1` the three tests skip with the reason;
  without it each passes 10 of 10. Decided with Edmond on 2026-09-29.

  Left open: the cause of the skew is not traced — every shared-fd run gave the
  majority to the second worker, which the idea of a busy worker winning does
  not explain; the measurements and the tracing step are in #329, together with
  the question whether #240's answer still holds. `telemetry/012` rests on the
  same premise in the second pass and has not failed yet; the same predicate
  applies if it does. Opening connections in a burst, so both workers drain one
  backlog, is an untried alternative to skipping.

- [ ] **`http_server_pause_listeners` may not stop accepting on the socket.** It
  calls the listen event's `stop`, and `libuv_listen_stop` in `ext/async`'s
  reactor only drops a reference count ("uv_listen doesn't have a stop
  function"); the pause tests check counters, not the fd (`h1/011`: "Don't
  check listeners_paused"). Found while reading for #329, not run: the first step
  is a test that stays connected-and-accepted while the listeners are paused.

## A WebSocket close is lost when the peer is still sending (#303 fallout)

- [x] **The 1013 close never reaches a client that overflowed the inbound
  cap.** `websocket/035-recv-queue-overflow` fails on Windows with
  `client saw close: NULL` while the handler side reports 1013 correctly, so
  the decision is right and the frame does not arrive. The shape is the one
  #287 named for HTTP/1: the client writes 24 frames, about 16 KiB, the server
  caps at 8 KiB and stops reading, queues the close and tears the transport
  down at `ws_session.c:1345` — and a socket closed with unread bytes in its
  receive buffer is reset rather than finished, which discards what this side
  has written. #288 gave HTTP/1 a lingering close for exactly this; the
  WebSocket overflow path has none.

  The cheap proof is the one that settled #287, and it settled this too: run
  outside run-tests, the test reads `NULL` on 5 of 10 runs; cut the client's
  write to 13 frames — just past the cap, nothing left unread — and it is 0 of
  10, while 60 frames is 7 of 10. The condition is the unread tail, not the
  volume. Windows shows it because a reset there is immediate; Linux may be
  hiding it behind timing rather than avoiding it.

  Fixed in #305 for a plaintext ws connection: the overflow teardown arms the
  same drain #288 built, on an upgraded H1 connection only — over H2 the session
  shares the connection with other streams. 0 of 20 after, and the whole phpt
  suite is 259 executed, 0 failed. Over wss it is still lost; see the TLS item
  below.

  Found while making the group build on Windows at all (#303), where it is the
  single failure out of 61 executed WebSocket tests.

- [x] **The lingering close has no TLS path, so wss and https keep losing what a
  reset discards.** Closed by #311: the TLS read drains into `read_buffer` and
  drops each chunk undecrypted, and the read stays armed while the drain is
  pending, a cancelled handler still unwinding included. `tls/017`, `tls/018` and
  `websocket/071` fail before and pass after; the flood that proves it on Linux is the client's write failing
  midway, since Linux still delivers the 413 or 1013 that arrived before a reset.
  Critic 2026-09-29: two findings left open by design. `stop()` closes no
  connection, draining or idle, until the server object is freed (the TODO in
  `http_server_do_stop`); frames pipelined before the 101 that trip a cap arm the
  drain without the FSM reaching CLOSING, so the deadline tick ends it and no
  close_notify is sent. Before: The drain is fed from the plaintext read paths alone —
  `http_connection_linger_note_inbound` is reached from `http_connection.c:1324`
  and `:1429`, and `http_connection_tls.c` does not mention the drain at all. A
  TLS connection that armed it would wait with nothing being read and close on
  the same unread bytes at the end, so `linger_begin` refuses there rather than
  spending the deadline for nothing. That refusal is the honest state and not
  the fix: a 413 over https and a 1013 over wss are lost exactly as #287
  described. Closing it means giving the TLS read FSM the drop-and-refresh hook
  the plaintext paths already have. Found by the review of #305.

- [x] **The permessage-deflate teardown has the same shape and no proof.** Proved
  and closed with #311 (#312): `websocket/072` fails the client's flood after the
  bomb on 10 of 10 runs before the teardown opens the drain, and passes after.
  Before:
  `pmce_error` queues a 1009 and returns -1 down the same path, so a peer whose
  compressed message overflows the cap should lose its close for the same
  reason. No run of it has been made to fail, so it is left as it stands rather
  than fixed on the resemblance. The proof, if it is wanted, is the one above:
  a bomb just past the cap against a flood well past it.

## Forms on every transport (#317, #318)

- [x] **#317 — the form of a request is built in one place, keyed as `$_POST` is.**
  `getPost()` parsed no url-encoded body, kept multipart names verbatim, and was
  empty over HTTP/2 and HTTP/3. `src/http_request_form.c` builds the form on first
  access; names go through `src/http_form_vars.c`, which follows
  `php_register_variable_ex` without interning keys, and `getQuery()` moved to it
  too, closing a leak of every distinct key. HTTP/1 and HTTP/2 feed multipart to
  the processor as DATA arrives; HTTP/3 buffers and parses in 256 KiB slices in
  the handler (CODING_STANDARDS 1.5). Evidence in the CHANGELOG entries.
- [x] **#318 — a form over a limit is refused, never shortened.** The multipart
  processor dropped field 101 onward and cut fields past 1 MiB silently; HTTP/1
  multipart ignored `setMaxBodySize()`; HTTP/3 capped a buffered body at a fixed
  16 MiB and handed a cut body to the handler as complete. Edmond, 2026-09-26: the
  silent drop is a bug; the answer is a refusal. Fields count against
  `max_input_vars`, a value is bounded by the body limit, and two states keep the
  refusals apart: `req->refused_status` for a body the transport refused (400 or
  413 before the handler on HTTP/1, a reset stream on HTTP/2 and HTTP/3, and every
  read of the body throws), `req->form_refused_status` for a form refused while it
  was built (the getters throw 400, the body stays readable). Review found HTTP/3
  pre-sizing a buffered body from the claimed Content-Length without a bound on a
  reactor and outside a bailout guard on a worker; both fixed (`h3/076`, `h3/077`).
  Open: a query string keeps `$_GET`'s rules, and a reset HTTP/2 or HTTP/3 stream
  carries no 400/413 to the client.
- [ ] **#322 — upload limits are compiled in.** 20 files and 100 MiB per file,
  reported as codes 100 and 102 that no `UPLOAD_ERR_*` constant names. Proposed:
  read `upload_max_filesize` and `max_file_uploads` as `max_input_vars` is read.
- [x] **#320 — `tests/h3client` never unblocked a flow-control-blocked stream.**
  It called `nghttp3_conn_block_stream` on `NGTCP2_ERR_STREAM_DATA_BLOCKED` and
  had no `extend_max_stream_data` callback, so an upload past the 256 KiB stream
  window hung about half the time from 500 KB and always from 1.1 MB, while
  aioquic sent 15 MB through the same server. #321 adds the callback: `h3/072`
  uploads past the window, and `h3/035` dropped its 20 MiB window workaround.

## Three defects found by the reviews of #305 and #310 (#313, #314, #315)

- [x] **#313 — run-tests reports only the second attempt of a retried test.**
  `is_flaky` (`run-tests.php:3118-3140`) marks a test for retry when its FILE
  section calls `disk_free_space`, `hrtime`, `microtime`, `sleep` or `usleep`;
  `is_flaky_output` (`:3143-3155`) retries any failure whose output holds
  "timed out", "connection refused" or four other strings, so a hung test is
  retried whatever it calls. A failed first attempt is discarded (`:2912-2914`)
  and the verdict is the second one, listed under `WARNED TEST SUMMARY`
  (`:3460-3469`) as "passed on retry attempt"; CI runs with `-g FAIL,BORK,LEAK,XLEAK`
  (`build-linux.yml:551`, `.github/scripts/windows/test_task.bat:46`), so the
  summary is the only trace. Counted on 2026-09-28 with the same regex: 352 of
  520 phpt files under `tests/phpt` match. `websocket/035` failed 5 of 10 runs
  outside run-tests while the suite was green (#305). Two halves, in order: a
  ratchet in CI first (count the warned summary, hold the number at today's
  baseline, refuse a rise), then the clock calls out of the tests in batches;
  a test that keeps one says why in a comment. A proof for an intermittent
  defect is written without those calls and with a failure line the output
  list does not match, as `035` is, and is run outside run-tests.

  The ratchet, PR 331: `.github/scripts/assert_no_new_retries.php` reads the
  warned summary of every gating phpt run and fails the job on a retried test
  outside `.github/scripts/retry-baseline.txt`. The baseline, from 89 green
  jobs on 2026-09-29: `core/023` on macOS release (2 of 15 jobs) and `tls/003`
  on Windows (2 of 29); no Linux job retried. Both are open defects, and an
  entry leaves the list when its test is fixed.

  The clock removal is dropped, by the Sage on 2026-09-29, at Edmond's
  request. The ratchet already refuses a new retry, and 354 rewritten files
  would still leave the retries `is_flaky_output` grants on "connection
  refused" and "timed out", which a network suite prints most. Instead, in
  order:
  - [x] `core/023` and `tls/003` wait on the event under a deadline instead of
    a fixed sleep; the baseline is empty. `core/023` read its buffer without
    waiting for the coroutine that fills it: 0 of 20 with that reader delayed
    200 ms, 20 of 20 after. `tls/003` stopped the server 1.5 s in whatever
    s_client was doing: `tls13: no` with the client delayed 2 s, `yes` after.
    Rewriting `023` found #332, a `stop()` lost while `start()` was still
    starting; fixed in the same PR, `core/079` red 3 of 3 before.
  - [x] CI patches php-src's `run-tests.php` with `git apply` to print the
    first attempt's diff before `goto retry`, and fails if the patch stops
    applying: PR 334. The step passed in all five build jobs; the `RETRY`
    block was shown locally with `-j2 -g FAIL`, since no CI run retried.

- [ ] **#314 — `compression/070` hung twice on one Windows job and passed on
  the rerun.** `tests/phpt/server/compression/070-encoder-pool-reuse.phpt`
  calls none of the clock functions, but its timeout text (`run-tests.php:1374`)
  matches `is_flaky_output`, so the failure the job printed was the second
  consecutive 120 s hang (`test_task.bat:50`). Not reproducible on a local
  Windows build: the test needs zlib, which that build lacks. No fix without a
  loop. On recurrence the job's `*.log` (already uploaded by
  `build-windows.yml:113-121`) is what to read: no output at all points at
  startup or at the first response, not at the pool reuse the test is named
  for.

- [ ] **#315 — the Windows fnmatch shim takes exponential time on a
  star-heavy hide pattern.** `win32_fnmatch_impl`
  (`src/static/http_static_path.c:25-46`) re-enters itself at every position
  for each `*`. `FNM_PATHNAME` comes off for a pattern holding `**` (`:354`),
  and off unconditionally for the two forms a directory pattern is expanded
  to, `<pattern>/*` and `*/<pattern>/*` (`:369-382`), so `*` crosses the
  whole request path there. Measured on 2026-09-28 with the shim compiled
  verbatim on Linux at -O2, cross-separator, against a path of `a`s that does
  not match: `*/a*a*b/*` 6 ms at 2048 bytes, `*/a*a*a*b/*` 4.3 s, `*/a*a*a*a*b/*`
  10 s at 512 bytes and over 30 s at 2048. A request path pins the reactor
  thread for that long, on Windows only and only under a pattern with three
  or more stars over a repeating literal, which is why it is hardening rather
  than a field defect. Fix: the iterative matcher with one saved star
  position, `*` stopping at a separator under `FNM_PATHNAME`; the `StaticHide`
  table (`tests/unit/static/test_static_hide.c`) is the check, and a step
  budget is refused as the alternative because giving up would have to mean
  hidden.
