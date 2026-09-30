# Principles

Ratified: 2026-09-30 (Edmond, from the baseline health check) · Amended: —

Trade-offs settled in advance. Within a group the order is the priority. A decision that
departs from a principle names it and gives the reason; a change is Edmond's and gets a
`dev/DECISIONS.md` entry.

## P1. Reliability

- **P1.1 Refuse over shorten.** Why: Edmond, 2026-09-26, on #318: a form cut past a
  limit is a bug and the answer is a refusal; the multipart processor had dropped field
  101 onward silently. Flips: never. Gate: the limit tests of each transport
  (`multipart/017`, `h2/067`, `h2/068`).
- **P1.2 A loud failure over a silent success.** Why: a Windows job stayed green on an
  absent extension (#271), and a dropped FULL wire recorded a 200 that never left the
  process (#351). Flips: a best-effort write on a socket closed on the next line.
  Gate: `assert_executed.php` and the retry ratchet in CI.

## P2. Performance

- **P2.1 A share-nothing per-thread core over shared state.** Why: CODING_STANDARDS
  §1.1–§1.3: the hot path has no locks, atomics or fences. Flips: the TLS handshake and
  the reactor/worker split (§1.3, `dev/DECISIONS.md` 2026-09-30); cross-thread
  aggregation above the core. Gate: held by discipline, no gate.
- **P2.2 No promise over a guarantee paid for on the hot path.** Why: Edmond,
  2026-08-24, on #240: fairness between workers is not promised, so no mutex or token
  sits on the accept path of every connection. Flips: a behaviour USAGE.md promises.
  Gate: held by discipline, no gate.

## P3. Security

- **P3.1 Reject over normalize** for security-relevant input. Why: CODING_STANDARDS
  §13c.2: a forgiving decoder lets the decoding layer and the using layer disagree.
  Flips: never in a security-relevant grammar. Gate: `static/003-static-security.phpt`
  and the libFuzzer targets in CI.

## P4. Process

- **P4.1 Defects over features.** Why: dev/PLAN.md, "Order of the open defects": #133,
  #106, #72, #48 and #6 wait behind the queue. Flips: a feature that unblocks defect
  work (81c0437: the suite's port races under `-j4`). Gate: held by discipline, no gate.
- **P4.2 A failing run over a plausible reading.** Why: dev/WORKFLOW.md: a bug fix lands
  with the test that fails without it; an item marked "reproduce" gets its failing run
  before any code. Flips: P4.3. Gate: held by discipline, no gate.
- **P4.3 A named gap over a test that cannot discriminate.** Why: three debts in
  dev/PLAN.md ("What a seam cannot reach, and why") stay uncovered because no shape
  reachable from PHP tells the versions apart. Flips: once a seam exists (libfiu), the
  test is written. Gate: held by discipline, no gate.
