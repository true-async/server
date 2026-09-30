# Decisions

Architectural decisions, newest first. Workflow rules live in `dev/WORKFLOW.md`.

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
