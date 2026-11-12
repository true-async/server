# Decisions

Architectural decisions, newest first. Workflow rules live in `dev/WORKFLOW.md`.

- 2026-11-12 Every Windows TCP listener keeps one AcceptEx at a time, as
  php-async #307 made it (S3.10, S3.14). Edmond, final: one mode everywhere,
  simpler than a rule that picks one.
  Why: a Windows agent's measurement of 2026-11-12 (`dev/BENCHMARKS.md`) found
  libuv's 32 faster only with one worker: a 1000-connect burst took 126-148
  ms against 569-1051 ms with one AcceptEx. With 2 workers one AcceptEx took
  94-111 ms against 130-142, with 4 workers 86-99 against 135-146, and with 32
  the last worker took 1000 of 1000 connections in 14 of 14 runs. One
  AcceptEx also keeps a paused listener's queue in arrival order (`h1/064`).
  Rejected: 32 for a listener that never pauses (the first decision of this
  day, implemented on `feature/listen-no-pause` in php-src, php-async and the
  server and not merged); 32 for a single uncapped worker only (faster for
  that case, one more rule to keep).
- 2026-11-12 A pool worker ended by a fatal error tears its server down without
  calling PHP or suspending, and the drain waits for and cancels the server scope
  through the C scope API, not through Scope methods (#439).
  Why: a bailout out of `Scope::awaitCompletion()` skips the `$this` pin of
  `zend_call_function()`, so the Scope object outlived the teardown and its
  handlers were cancelled only at request shutdown; under exhausted memory the
  methods raise a second fatal. With the pin gone the teardown disposes the
  scope during php-async's bailout unwinding, which needed php-async to take a
  coroutine queued during the unwinding off the queue (php-async 9384c78).
  Sage, final: the Critic's remark stands; a workaround in the server was
  rejected (a scope kept past the teardown leaves the handlers alive when the
  bailout is raised on start() itself).
- 2026-11-12 The pool parent tells a worker ended by a fatal error from one that
  never started (exception code `POOL_WORKER_FATAL_CODE`); `start()` still
  answers false for either (core/088, 115, 116). Sage, final. Whether a
  worker's fatal changes the process exit status, and whether `start()` stays
  false for a pool that kept serving, are left to their own issue.
- 2026-11-11 After a bailout the pool parent joins its worker threads by blocking
  its thread (1 ms polls until the pool's reference count is 1) and never
  suspends or calls PHP again (#446).
  Why: the scheduler unwinds each coroutine once and never resumes it, and under
  exhausted memory the engine refuses every call; the request-shutdown free of
  the server releases the reactor pool and the worker shells before module
  shutdown joins the threads. Sage.
  Rejected: not waiting (a booting worker reads freed memory); driving the
  reactor by hand; counting live handlers (blind to tasks the closed pool
  drops); a php-async change.
  The wait is bounded: past `setShutdownTimeout()` plus 10 s the process ends
  with `_exit(255)`, the status of a fatal error. Why: a thread blocked on the
  parent (a bootloader receiving from it) never leaves, and freeing under it is
  no option (Critic, S7).
- 2026-10-06 A Windows worker pool keeps one reactor per worker, each on its own
  duplicate of the shared socket, although connections spread unevenly: under a
  light `Async\delay(200)` load worker 0 accepted none of 30 (#375).
  Why: under CPU load every worker works; 512 connections, 32 clients, 30000
  SHA-256 rounds per request gave 12.88 / 21.45 / 36.13 req/s and 1.00 / 1.99 /
  3.90 cores for 1 / 2 / 4 workers (Windows Release ZTS, 3 runs each, median).
  The cause of the uneven split was not looked for. Edmond.
- 2026-10-01 A config method whose value nothing reads is a tombstone: it throws
  `HttpServerRuntimeException` on any argument, naming what does the job, for one
  minor release; its getter throws too. Twelve methods (#393), `enableHttp2` and
  `enableWebSocket` brought to the same shape.
  Why: an accepted value still makes the getter misreport the server
  (`isHttp2Enabled()` false while h2c is served). Sage, Final.
  Rejected: accepting the value that is already true (enableHttp2's old pattern);
  wiring `enableTls()` to the constructor's listener (a feature, a second spelling
  of `addListener(..., true)`); E_DEPRECATED through `@deprecated`, which hides
  the replacement behind a generic notice.
  Principle: P1.2, P4.1.
- 2026-10-01 `getTelemetry()` drops keys that were always 0 rather than wiring them
  in the same change; byte counters are a separate feature (#396).
  Why: P4.1, the defect is the constant, the counters are new work. Sage, Final.
- 2026-09-30 php-async clamps each TransmitFile pass to the file size (1521d69); it stays.
  Why: a probe (Windows 11) found the file pointer 32 KiB in after any larger send, and a
  count or start past EOF fails with WSAEINVAL, sending none of a 4 KiB file (`h1/065`).
  Rejected: counting from the file pointer (php-async 4102d7f).
- 2026-09-30 A failed or short HTTP/1 file body ends the connection through the
  framing-lost path of `http_request_finalize`; a deferred start the loop refuses
  is answered before anything is written, with SEND_FILE_REFUSED.
  Why: `should_continue=false` alone still served the pipelined request; closing
  from a refused start ran inside `llhttp_execute` (Critic, S2).
  Rejected: closing the connection on a refused start; a 500 written by the engine.
- 2026-09-30 The reactor/worker split is the second sanctioned cross-thread off-load
  (CODING_STANDARDS §1.3), beside the TLS handshake.
  Why: §1.3 forbade any stage crossing cores while §1.5 and the reactor pool already
  did it; #350 builds pooled compression on the split.
  Rejected: removing the pool to keep §1.3 whole.
  Cost: a mailbox post per request and per response; opt-in via
  `TRUE_ASYNC_SERVER_REACTOR_POOL=1`.
  Principle: departs from P2.1 by its own flip.
- 2026-09-30 Test strength is measured with Mull on the cmocka binaries, weekly in CI;
  faults are injected with libfiu (fault points in C, `fiu-run` over the phpt suite
  under ASan, nightly).
  Why: the baseline health check found no mutation tool and no seam for failing
  `open`, `read`, `sendfile`, `accept` or allocation.
  Rejected: mutating the extension through phpt (a rebuild and an area run per mutant,
  about ten mutants per 15 minutes); a hand-written `--wrap` mock layer.
  Cost: two tools in CI; the nightly fault run checks crashes, hangs and leaks, not
  output.
- 2026-09-30 Scenario tests wait until the defect queue is empty; when they come, they
  are given/when/then helpers in a shared phpt `.inc`, not a Gherkin runner.
  Why: a second runner would drive a separate server process; phpt already runs the
  server in-process.
  Rejected: Behat.

- 2026-10-06 Pooled HTTP/3 delivery (#351): FULL is coroutine backpressure, not
  a discard policy. Enqueue transfers ownership but does not count a response.
  The reactor selects one terminal outcome at clean QUIC stream close (data and
  FIN acknowledged), or failure. The original worker generation consumes that
  result and its immutable request snapshot once. No confirmed status is the
  fifth, `responses_undelivered_total`, bucket; the five sum to `total_requests`.
  Principle: P1.2, within P2.1's existing reactor/worker exception.
  Why: forced refusal timed out the client while recording 200 and ten bytes,
  with zero dropped wires. A parked wire was not delivered either.
  Rejected: keep dropping FULL but count the drop; use HttpRequest lastref as
  completion; put failure notifications in the same full mailbox; hold the PHP
  coroutine/response until peer ACK.
  Cost: a persistent delivery record and copied access identity per request;
  capacity gates per worker/reactor pair; await resources only on pressure.
  Command slots remain bounded, but an internal producer block may allocate:
  moodycamel try_enqueue refused such a block even below logical capacity.
  Enqueue under an explicit slot reservation distinguishes actual allocation
  failure from FULL. Control/completion publication allocates nothing.
