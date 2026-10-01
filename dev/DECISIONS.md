# Decisions

Architectural decisions, newest first. Workflow rules live in `dev/WORKFLOW.md`.

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
