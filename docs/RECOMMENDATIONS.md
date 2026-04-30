# Recommendations

Process and workflow guidance for contributors (human or AI). These are not
code-shape rules — for those see [`CODING_STANDARDS.md`](CODING_STANDARDS.md).
The points below are about how to plan, prioritise, debug, and integrate
work in this project.

---

## 1. Bugs first, always

Real bugs (UAF, UB, data races, security findings) **outrank** polish,
refactors, perf wins, and infrastructure work — regardless of size.

When the open list mixes a known UAF with a "small safe item", the UAF
goes first. Do not phrase the choice as "small or large?" — phrase it as
"bug or not?". Smaller polish items remain easy to revert later;
unfixed bugs become production incidents.

---

## 2. Don't dismiss bugs as "upstream"

When a test fails, a hang appears, or an assertion fires, do not label it
"ext/async bug", "php-src problem", "not our code". Even when the symptom
surfaces in upstream code, the trigger is our usage — investigate our
callbacks, refcounts, lifecycle, and ordering first.

A bug is ours until proven otherwise by a stand-alone reproducer in a
clean PHP + ext/async build with TrueAsync Server **not loaded**.

If the reproducer is genuinely upstream, fix it upstream — TrueAsync and
php-async are projects we own (see §3). Do not shim, do not silently
`XFAIL`, do not skip.

---

## 3. Fix gaps upstream when we own the upstream

If an upstream dependency has an API gap and we own that upstream project,
extend the upstream API rather than working around it in the consumer.
TrueAsync (`zend_async_API.h`, `ext/async/libuv_reactor`) is ours. When
this extension consumes a TrueAsync API and finds it missing a function,
the plan must include the upstream addition as an explicit step. No
shims, no manual `socket()+bind()` workarounds, no "small hack until
later".

Asymmetry in an existing API surface (e.g. UDP exposes everything except
`bind`) is a smell to investigate, not a boundary constraint to
work around.

---

## 4. Failing tests are signals — never delete

A failing test means exactly one of:

1. The code under test has a real bug → fix the code.
2. The test itself is wrong → rewrite the test, **keep the case**.

Never delete or `skip` a failing test "because it flakes". If a defensive
code path cannot be exercised with the current test harness, that is a
separate signal: either the defensive code is dead (rethink it), or the
harness needs another tool (a different client, lower-level socket
control, fault injection). Hiding the failure is not an option.

---

## 5. Verify scenario reachability before defensive code

Before implementing a defensive branch from a plan, trace the data path
end-to-end and confirm the scenario is actually reachable on our stack
(libuv + the kernel + our FSM + ext/async). For each defensive step ask:

1. Which exact code path makes the observed condition true?
2. Is that scenario already covered by an existing mechanism (timeout,
   gate, flag)?
3. If not, is there a realistic, reproducible case?

If the answer to (3) is "no", the plan step is dead-on-arrival — discuss
with the plan author rather than ship dead defensive code.

---

## 6. Follow architectural decisions strictly

When a specific API or mechanism is chosen during a design Q&A
("use the ext/async API for async-handle from `php_stream`"), the
implementation **must** use exactly that mechanism. Do not:

- Turn the chosen path into a TODO and ship a workaround
  (e.g. raw `write(2)` after `php_stream_cast`).
- Postpone an already-agreed mechanism without explicit permission.
- Justify a workaround with "the API was hard to figure out" — read the
  upstream sources, find a pattern in the repo, ask.

If during implementation the chosen path turns out unworkable, **stop and
escalate** before writing the workaround.

---

## 7. Scope discipline

Do not introduce changes that were not asked for.

- When asked to **analyze** (profile, benchmark, log inspection), reply
  with the analysis only. Do not propose "let's also disable flag X and
  re-measure" unless explicitly asked.
- When asked "is feature Y faster now?" — measure and report. Do not
  spontaneously launch follow-up optimizations.
- Decisions to flip flags, rebuild with different options, or change
  configuration belong to the maintainer.

Bug fixes do not need surrounding cleanup. One-shot operations do not need
helper abstractions. Three similar lines beat a premature abstraction.

---

## 8. Git workflow

- "Merge" means **merge commit**. When asked to merge a branch, run
  `git merge` (preferring `--no-ff` to preserve topology). Never
  silently substitute a rebase. Use rebase only when explicitly asked
  for rebase or for linear history.
- Prefer new commits over `--amend` when iterating after a hook failure.
  A pre-commit hook failure means the commit did not happen — amending
  in that state mutates the **previous** commit and can destroy work.
- Stage files explicitly (`git add path/to/file`) rather than `git add
  -A` / `git add .`, to avoid accidentally committing secrets or large
  binaries.
- Never bypass hooks (`--no-verify`, `--no-gpg-sign`) without an explicit
  instruction.
