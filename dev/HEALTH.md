# Health

## How this project is checked

Maturity: mature (release 0.16.0 on 2026-09-26; laravel-spawn depends on it)
Check day: Monday
Time budget: 15 minutes per run
Suite: `run-tests.php -n -j4 tests/phpt/`, 537 tests, 110.3 s on the dev laptop (WSL2, 16 cores,
  release build, 2026-09-30); `ctest` in tests/build, 21 tests, 0.41 s (same day)
Coverage: CI lcov on the Linux release ZTS job, `docs/coverage-baseline.json`: 21,203 of 25,340
  lines, 1,624 of 1,776 functions (main at 3425391, 2026-09-30); phpt only, ctest not instrumented
Mutants: no tool yet
Practices: fault tests (5 libFuzzer targets, Http11Probe, Autobahn, h2spec, scheduler chaos), CI;
  missing: mutation tool, dev/PRINCIPLES.md, specification tests with dev/TESTING.md
Slices: src/core + src/http1, src/http2 + src/http3, src/static + src/compression + src/formats,
  the rest (room, websocket, log, grpc, the class files); next: src/core + src/http1
Rotation: 5, 7, 6, 8, 9, 10; last run: all six on 2026-09-30 (baseline)
Known dark places: src/http_server_class.c (644 of 2,682 lines not run), src/log/http_log.c
  (344 of 1,118), src/room/room_hub.c (299 of 886), the non-Linux HTTP/3 branches (no CI job
  builds HTTP/3 off Linux), src/core/thread_queue.cc (not measured: coverage flags reach CFLAGS only)

## Open findings

- 6 src/core/http_connection.c:2744,2833: a failed or short HTTP/1 file body keeps the connection alive; the next response follows a short body (PLAN 12a)
- 6 src/http1/http1_sendfile.c:297-309: a short plaintext sendfile reports success (PLAN 12a)
- 6 src/send_file.c:617,758: a failed defer schedule finalizes with nothing written and keeps the connection (PLAN 12a)
- 6 src/http2/http2_static_response.c:593-603: a truncated or failed file body ends with END_STREAM, not RST_STREAM (PLAN 12b)
- 2 .github/workflows/build-linux.yml:485: no leg passes --enable-http-server-test-hooks; 20 phpt skip on every CI leg (PLAN 17)
- 6 src/formats/multipart_processor.c:580-581: fflush/fclose results ignored, a failed flush leaves the upload OK (PLAN 18)
- 8 src/formats/multipart_parser.c:25, multipart_processor.c:37: the shipped POSIX build uses libc malloc, tests and fuzz use emalloc (PLAN 19)
- 6 src/http3/http3_stream.c:123-125: unbounded retry of reactor_pool_post_exec once the reactor leaves RUN (PLAN 20)
- 2 tests/unit/http1/test_parser_security.c:625: 23 of 55 cases end in `(void)result`, the parse outcome is not asserted (PLAN 21)
- 2 .github/workflows/build-linux.yml:928-966: HTTP3Packet and HTTP3SlotRelease are not built in CI (19 of 21 ctest targets run) (PLAN 22)
- 2 tests/unit/http3/test_http3_packet.c:68: the stateless reset is checked through a stub return, the bytes are not inspected (PLAN 22)
- 5 src/http_etag.c:40,116, src/http_range.c:135-141, src/http_date.c:97, src/static/http_static_path.c:111,118,171,183: no test fails when these are changed (PLAN 23)
- 5 tests/phpt/server/static/011-static-range.phpt:115-123: the HTTP/1 range body is never compared (PLAN 23)
- 5 src/compression/http_compression_negotiate.c:145, http_compression_request.c:182: `x-gzip` appears in no test (PLAN 23)
- 8 src/http_server_config.c:2531,2646,2676,2834: setWriteBufferSize, enableProtocolDetection, enableTls, setAutoAwaitBody store a value nothing reads (PLAN 24)
- 7 src/http_server_class.c:5620-5622: getTelemetry() returns bytes_received, bytes_sent, errors as a literal 0 (PLAN 24)
- 6 src/http_server_class.c:4111: CODEL_TARGET_MS parsed with strtol and no endptr check (PLAN 24)
- 6 src/http_server_class.c:2218-2223: an accept error (EMFILE) is dropped with no log and no counter (PLAN 12)
- 7 config.m4:488: coverage flags reach CFLAGS only; thread_queue.cc and the ctest run are not measured (PLAN 25)
- 3 tests/phpt/server/tls/007-tls-pipelining.phpt:97: a fixed watchdog sleep sets the test time; 24 files sleep 1 s or more in one call (PLAN 26)
- 2 tests/phpt/server/core/020-builtin-worker-pool.phpt:8: workers > 1 on Windows has no test; 56 phpt skip there on SO_REUSEPORT (PLAN 27)
- 2 tests/phpt/server/static/009-static-symlink-owner.phpt:13, 021:15: "tracked gap" skip on Windows names no issue (PLAN 27)
- 2 tests/e2e/*.phpt: 11 tests no workflow runs (decision pending)
- 2 tests/fuzz/h3_datagram_fuzz.c:123: built and run by nothing, exercises no project source (decision pending)
- 8 src/core/http_protocol_strategy.h:85,90: send_response and reset slots never called; six empty bodies (PLAN 28)
- 8 include/php_http_server.h:417-456: _http_listen_event_t and _http_server_t unused (PLAN 28)
- 8 src/core/thread_queue.cc:197-275: the SPSC queue and deps/concurrentqueue/readerwriterqueue.h serve only a unit test (PLAN 28)
- 8 config.m4:126,134,363,364,480,490: HAVE_LLHTTP always set; HAVE_WSLAY, HAVE_NGTCP2, HAVE_NGHTTP3, HAVE_CMOCKA, HAVE_COVERAGE never read (PLAN 28)
- 8 src/http3/http3_dispatch.c:53-64: H3_TRACE "temporary" stderr tracing, set by nothing (PLAN 28)
- 8 src/compression/http_compression.c:66 and 12 more functions with no caller (pass 8 list, 2026-09-30) (PLAN 28)
- 7 src/http_server.c:196, include/php_http_server.h:388,1425,1431, src/http2/http2_static_response.c:39, src/core/http_protocol_strategy.c:227, src/http_server_class.c:4885: TODO without an owner (PLAN 28)
- 9 CHANGELOG.md:76: says WebSocket is absent on Windows; 5557cd9 built it (PLAN 29)
- 9 docs/CODING_STANDARDS.md:144: cites docs/PLAN_REACTOR_POOL.md, which does not exist; :155-159 still calls pooled compression pending (PLAN 29)
- 9 dev/WORKFLOW.md:55-57: the local command runs tests/phpt/server/, CI runs tests/phpt/ (PLAN 29)
- 4 dev/PLAN.md: step lines for the worker-path compression, #322 and #315 left open after their fix; the Critic note of the #311 step still says stop() closes no connection (PLAN 29)
- fine 2 tests/phpt/server/h2/009-h2-h2spec-gate.phpt:9: skips in phpt; conformance.yml:102-113 runs h2spec and fails on any failure
- fine 2 tests/unit/http1/test_parser_security.c:1009,1060: no assertion; h1/019-host-validation.phpt rejects the same inputs end to end
- fine 5 src/http2/http2_session.c:181: no cmocka or phpt test; the weekly h2spec job fails on it
- fine 5 src/static/http_static_path.c:125,138: covered by static/003-static-security.phpt:102-108
- fine 7 src/core/reactor_pool_test_hooks.c: near-zero coverage; compiled out of release builds (the file's header)

## Journal

### 2026-09-30

Passes: 1–10 (baseline, whole project); 1, 2, 5–10 by health-auditor agents, 3 and 4 by the main
  model, all findings weighed by a Sage that read the code for each claimed defect
Numbers: phpt 537 tests, 512 passed, 25 skipped, 110.3 s; ctest 21 of 21, 0.41 s (dev laptop,
  main at 3425391); coverage 21,203 of 25,340 lines (CI, same commit); mutants: none, no tool
Findings: 35 NEW, listed under Open findings; 0 resolved (no earlier check)
Refuted: an EMFILE accept error does not stop the listener (php-async re-arms it); the finding
  keeps only the missing log and counter
Looks bad but is fine:
- chaos.yml:245-269 does not gate the scheduler matrix: order-sensitive EXPECT blocks, tracked in #43
- the TAS hooks run on the debug leg only: build-linux.yml:483-484 keeps the release leg as shipped
- core/018 skips on GITHUB_ACTIONS: CI timing jitter, #48; its load failure is PLAN 13
- the static cache compares its TTL with wall-clock time: a backward step only lengthens
  staleness, and PLAN 12a makes a stale size safe
- a failed H3 owner-mailbox post drops the datagram: QUIC retransmits, a counter records it
- every static open failure answers 404: it does not reveal a restricted file
- on_outbound_drain and tls_zc_write_done_cb have one implementation each: they keep the core
  free of WebSocket and HTTP/1 sendfile code without an #ifdef
- each vendored library sits behind one wrapper file, the only backend of its subsystem
- the room skip on the shared fd and the run-tests retries are recorded decisions in dev/PLAN.md
  (#329, #313)
- the seven departures from "a fix lands with a failing test" each name their reason in PLAN or
  CHANGELOG
Strategy signals: no closed step reopened; four `[~]` steps older than four weeks ("Migration",
  "#240", the php-async bump, "WINDOWS_X64_ZTS_RELEASE is green on an absent extension"); the
  step lines and the ordered list drifted on five items; the plan has no 23.10 review lines
Plan: items 12a, 12b and 17–29 added to "Order of the open defects"
Next: 5, 7
