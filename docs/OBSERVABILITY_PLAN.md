# Observability plan — telemetry API + logging redesign

Development plan for issue **#5** (Telemetry API), reshaped to the agreed scope:
**no embedded servers.** Metrics are read through a plain PHP array API; logs
are fanned out to pluggable sinks. Anyone who wants Prometheus/OTLP exposes it
themselves in userland on top of the array.

_Status: planning. Last updated: 2026-07-10._

## Scope decisions (locked)

1. **No built-in HTTP endpoints, no embedded exporters.** No Prometheus text
   format in C, no OTLP client in core. `getTelemetry()` returns an array; the
   user builds any exposition format in PHP.
2. **Metrics = aggregate of the counters that already exist.** The server
   already keeps per-worker, lock-free (no-atomic) counters
   (`http_server_counters_t`, `php_http_server.h:747`). The work is to make them
   walkable across workers and expose them — not to invent a new metrics stack.
3. **Logs already have a solid core** (`src/log/http_log.*`): OTel severities,
   message templates, structured attrs, trace context, async non-blocking
   writer. The work is multi-sink fan-out + formatters + external transports,
   not a rewrite.
4. **Stats are opt-in.** Disabled → the telemetry API throws. Counter bumps stay
   always-on (single cheap increments); the gate is on the aggregation API +
   slab, not on the hot-path increment.

---

## Development order (efficient sequence)

Ordered for one developer to minimise rework and context-switching:
dependency-correct, each part shipped as a coherent unit, the **risky
foundational refactors done early** (against a clean profiling baseline) so
nothing built on top has to be redone.

**Part A first** — it is smaller, self-contained, and barely touches the emit
hot path, so it delivers a complete, testable telemetry API quickly and
establishes a coverage baseline. **Part B second** — the larger refactor, built
on nothing from A.

| Step | Stage | Depends on | Risky? |
|------|-------|-----------|--------|
| 1 | A1 — stats registry + slab skeleton ✅ | — | no |
| 2 | A2 — point workers at their slot ✅ | A1 | ⚠ hot path → **profiler** |
| 3 | A3 — config gate `setStatsEnabled` ✅ | A1 | no |
| 4 | A4 — `getStats()` aggregate ✅ (new method; `getTelemetry`/`resetTelemetry` kept as-is) | A2, A3 | no (off hot path) |
| 5 | A5 — fill counter gaps | A2 | ⚠ commit hot path → **profiler** |
| 6 | B1 — single → multi-sink (internal) | — | ⚠ emit hot path → **profiler** |
| 7 | B2 — JSON + logfmt formatters | B1 | no |
| 8 | B3 — pretty console formatter | B2 | no |
| 9 | B4 — `setLogSinks` config | B1, B2 | no |
| 10 | B5 — external transports | B1 | ⚠ blocking I/O vs reactor → **profiler** |
| 11 | B6 — structured access log | B1, B2 | ⚠ per-request emit → **profiler** |
| 12 | B7 — `onLog` PHP hook | B1 | no |
| — | A6 — duration histograms | A4 | deferred |

---

## Quality gate (applied at the end of every stage)

Each stage below ends with this checklist — the definition of done. Rules
reference [`CODING_STANDARDS.md`](CODING_STANDARDS.md) §4 (comments) and §6
(`const`); coverage is compared against
[`docs/coverage-baseline.json`](coverage-baseline.json).

- [ ] **Code quality** — guard-clause early returns, ≤3 indent levels, no dead
      params, no `else` after `return`, cleanup via `goto out`; hot path
      unchanged where the stage claims "no hot-path cost".
- [ ] **No duplicated logic** — reuse the existing helper/pattern instead of
      copy-paste (each stage names the thing it must reuse). Grep before adding a
      second copy.
- [ ] **`const` correctness** — `const` on every read-only value: `const T *`
      params, fixed locals (`T *const x = …`), literals as `const char *`,
      getters return `const T *`. Strictest form that fits; never cast away
      outside a documented FFI boundary.
- [ ] **Comments reviewed** — default no comment; keep only non-obvious WHY
      (invariant / workaround / edge case). Delete WHAT-restatements, task/PR
      refs, "used by X" notes.
- [ ] **Tests written** — new unit/integration tests covering this stage's
      behaviour, including the failure/edge paths it introduces.
- [ ] **Coverage checked** — new code is exercised; no coverage regression vs the
      baseline (refresh the baseline only with `[skip ci]` per house rule).
- [ ] **Build & verify** — clean build (no new warnings), tests green,
      ASAN/UBSAN clean on the touched paths.
- [ ] **Profiler** *(risky stages only)* — before/after profile on the named hot
      path shows no throughput/latency regression (median of runs, apples-to-
      apples). Required for every ⚠ stage in the order table.

---

## Part A — Statistics API

### Model

Per-worker counters spin independently in each worker thread (already true, no
atomics). A process-wide **contiguous slab** holds one cache-line-aligned slot
per worker, allocated once when the worker count is known. Each worker points
its hot path at its own slot. The API walks the slab from any thread and reads
counters directly — no CAS, no per-read lock. A mutex is taken **only** when a
worker claims or releases a slot. Mirrors the existing `g_worker_registry`
(`http_server_class.c:2174`).

Return shape:

```php
[
  'enabled' => true,
  'workers' => [ 0 => ['requests_total'=>…, …], 1 => […] ],
  'totals'  => ['requests_total'=>…, …],   // summed across workers
]
```

---

### Stage A1 — stats registry + slab skeleton  _(step 1)_

Goal: `http_stats_slot_t` (cache-line-aligned, padded — no false sharing between
workers spinning adjacent slots) and `http_stats_registry_t`
(`g_stats_registry`): one contiguous `slot[workers]`, free-slot bitmap, mutex
for claim/retire only, `create`/`free`/`claim`/`retire`/`walk`.

Files: new `src/core/stats_registry.{c,h}`; bring-up hook in
`http_server_start_pool` / `http_server_reactor_pool_up`; teardown in the
matching pool-down path.

Acceptance: slab allocated once at pool start sized to worker count; single
worker / non-pool = slab of 1; claim/retire under the mutex; walk touches no
lock.

**Quality gate** — ✅ done (indexed `capacity()`/`at()` in place of a `walk`
callback; selftest lives in the existing internal-selftest TU):
- [x] Code quality (guard clauses, no partial-init leak — slab checked before
      the registry alloc)
- [x] No duplicated logic — reuses `worker_registry` idioms (mutex + atomic slot
      flags + claim/retire scan)
- [x] `const` — `const` registry on read-only params; documented const-cast in
      `http_stats_slot_active` for the non-const Windows atomic load
- [x] Comments reviewed (threading contract in the header, mirrors
      `worker_registry.h`; WHY-only in the impl)
- [x] Tests — `tests/phpt/server/telemetry/001-stats-registry.phpt`: full-table
      claim, overflow refusal, slab-of-1, write/read, retire + recycle-zeroing
- [x] Coverage checked (selftest hits every function + both cap paths)
- [x] Build & verify — clean build; **valgrind-clean** (0 leaked / 0 invalid).
      Full ASAN rebuild deferred (alloc module; valgrind covers the heap paths)

### Stage A2 — point workers at their slot  _(step 2 — ⚠ profiler)_

Goal: on worker-clone bring-up, `server->counters = &slot->counters` so the slab
is the single source of truth (no duplicated counter storage). Move the
telemetry fields now on `http_server_object` (sojourn/service/TLS/parse-error,
`http_server_class.c:368-411`) into the slot.

Files: `http_server_class.c` (clone init + `http_server_transfer_obj` LOAD).

Acceptance: hot path unchanged (conns still cache `&server->counters`); a
single-worker server still works with its slot; no double-counting.

**Quality gate** — ✅ done (via a `counters_live` pointer: `&counters` for a
standalone/parent server, the slab slot for a pool worker — cleaner than moving
every field and keeps single-worker zero-churn):
- [x] Code quality (hot path unchanged — bumps still go through the cached
      `conn->counters` pointer; only `http_server_counters()` + a cold admission
      read repointed)
- [x] No duplicated logic — counters live in one place per server
      (slot for workers, embedded otherwise); access consolidated on `counters_live`
- [x] `const` — snapshot reader takes `const http_stats_slot_t *`
- [x] Comments reviewed (counters_live/stats_up/stats_down WHY; stale
      "cache &server->counters" note fixed)
- [x] Tests — `telemetry/002-stats-slab-workers.phpt`: 2-worker pool, real
      traffic, `active_slots == workers` and per-slot `total_requests` sums to
      requests served (bumps land in the slab, not an embedded copy)
- [x] Coverage checked (pool path + single-worker embedded path both exercised)
- [x] Build & verify — clean build; 5 telemetry-read/reset/counter tests +
      reactor_pool + pool lifecycle all green
- [x] **Profiler** — 4-worker H1 wrk: **median 181.8k vs 182.6k RPS baseline**
      (−0.4%, inside ±20% loopback noise) — no regression

### Stage A3 — config gate `setStatsEnabled`  _(step 3)_

Goal: `HttpServerConfig::setStatsEnabled(bool)` — distinct from
`telemetry_enabled` (already means W3C trace-context ingestion — do **not**
overload). Off → slab not allocated, API throws.

Files: `http_server_config.c`, `stubs/HttpServerConfig.php` (+ arginfo regen).

Acceptance: default off; enabling before `start()` allocates the slab.

**Quality gate** — ✅ done (flag on both the mutable config and the frozen
worker config; slab creation gated in `start_pool`):
- [x] Code quality (mirrors `setRequestScope`, `config_check_locked` guard)
- [x] No duplicated logic — reused the config setter/getter pattern
- [x] `const` — `const http_server_config_t *` for the `start_pool` gate read
- [x] Comments reviewed
- [x] Tests — `telemetry/003`: round-trip, default-off, no slab when disabled
- [x] Coverage checked
- [x] Build & verify (green)

### Stage A4 — `getTelemetry()` / `resetTelemetry()`  _(step 4)_

Goal: read path. Check `enabled` (else throw), walk `[0..slot_count)`, skip
inactive slots (acquire load on a per-slot `active` flag), sum with plain 64-bit
loads — **no CAS, no read lock**. Build `{enabled, workers:[…], totals:{…}}`.
Reset = memset active slots.

Files: `http_server_class.c`; stub docs in `stubs/HttpServer.php`.

Acceptance: called from any thread; disabled → exception; sums match a
single-worker baseline.

**Quality gate** — ✅ done (implemented as a **new** `getStats()` method that
throws when disabled + returns `{enabled, workers, totals}` — `getTelemetry()`
stays a per-server view so its tests/contract are untouched):
- [x] Code quality (early throw + `RETURN_THROWS`; no lock on the read path)
- [x] No duplicated logic — one `stats_counters_to_zval` helper feeds both the
      per-worker entries and `totals`; word-wise `stats_counters_add` stays
      correct as A5 adds fields
- [x] `const` — `const http_stats_slot_t *`, `const http_server_config_t *`,
      `const http_server_counters_t *` on the readers
- [x] Comments reviewed ("stale-by-one aggregate is fine" WHY documented once)
- [x] Tests — `telemetry/004`: disabled throws; single-worker fallback; 2-worker
      pool aggregate with `totals.total_requests` == requests served
- [x] Coverage checked (both getStats branches + throw)
- [x] Build & verify — clean; 8 config/telemetry regression tests green

### Stage A5 — fill counter gaps  _(step 5 — ⚠ profiler)_

Goal: add the simple missing counters on paths that already run —
`responses_2xx/3xx/4xx/5xx_total` (bump at response commit), per-protocol active
gauges `conns_active_h1/h2/h3` (++ create / -- close).

Files: response-commit path; conn create/close per protocol.

Acceptance: numbers reconcile with `total_requests` and `active_connections`.

**Quality gate**
- [ ] Code quality (single increments, no branchy hot path)
- [ ] No duplicated logic — **one** status-class classifier helper called from
      every protocol's commit; not copy-pasted per protocol
- [ ] `const`
- [ ] Comments reviewed
- [ ] Tests — per-protocol counters reconcile with totals
- [ ] Coverage checked
- [ ] Build & verify
- [ ] **Profiler** — response-commit path RPS unchanged vs pre-A5 baseline

### Stage A6 — duration histograms  _(deferred)_

Bucketed latency histogram beyond the current sum/max/samples. Own stage, own
gate (incl. profiler — it adds to the per-request path), only if a percentile
consumer appears.

---

## Part B — Logging redesign

### Problem

Logs write to one destination in one format; the sink and formatter are
process-wide globals (`g_writer`/`g_formatter`, `http_log.c`). Nothing routes to
external systems; there is no JSON, no pretty console, no access log.

### Target

One record fans out to several destinations at once, each with its own format
and severity floor (Serilog *sinks* / zap *cores* / OTel Collector exporter
list) — one sink failing does not block the others.

---

### Stage B1 — single → multi-sink (internal, no behaviour change)  _(step 6 — ⚠ profiler)_

Goal: `http_log_sink_t = { severity_floor, formatter+ud, transport_write_fn+ud,
own async_io/pending/drop-counter }`. `http_log_state_t` holds an inline array
of sinks (cap ~8); fast gate = `min(floor)`. Emit formats once per distinct
formatter, fans out to passing sinks. De-globalize `g_writer`/`g_formatter` into
the sink. Today's single stream becomes exactly one sink.

Files: `src/log/http_log.{c,h}`.

Acceptance: byte-for-byte identical output to today with one configured stream;
drop-count + graceful-drain preserved per sink.

**Quality gate**
- [ ] Code quality (**emit hot path**: gate still one branch; format-once cache)
- [ ] No duplicated logic — per-sink writer **reuses the current
      `default_writer`/pending/drain machinery** moved into the sink, not a
      second copy; one drain-wait helper for all sinks
- [ ] `const` — `const http_log_record_t *` everywhere it is read
- [ ] Comments reviewed (keep the libuv buffer-ownership WHY; drop the rest)
- [ ] Tests — single-sink byte-identical golden; multi-sink fan-out; drain
- [ ] Coverage checked
- [ ] Build & verify (ASAN clean)
- [ ] **Profiler** — emit path cost unchanged when logging at INFO under load
      (the fan-out must not regress the disabled/one-sink common case)

### Stage B2 — JSON + logfmt formatters  _(step 7)_

Goal: `json` (one line, OTel Logs field names:
Timestamp/SeverityNumber/SeverityText/Body/Attributes/TraceId/SpanId) and
`logfmt` (key=value). `plain` stays.

Files: `http_log.c`.

Acceptance: valid JSON per line (escaping correct); logfmt parses in Grafana.

**Quality gate**
- [ ] Code quality
- [ ] No duplicated logic — **one** attribute-iteration helper feeds plain/json/
      logfmt (they differ only in per-attr rendering); shared timestamp format
- [ ] `const`
- [ ] Comments reviewed
- [ ] Tests — golden output per formatter; JSON validated; escaping edge cases
- [ ] Coverage checked
- [ ] Build & verify

### Stage B3 — pretty console formatter  _(step 8)_

Goal: `HH:MM:SS.mmm  LEVEL  [worker#/scope]  message  key=val …`. Fixed-width
colored level badge (INFO green / WARN yellow / ERROR red / DEBUG dim). Dimmed
timestamp, keys dim / values bright. Auto-color on TTY; honor `NO_COLOR` /
`CLICOLOR_FORCE`; color off to file/pipe.

Files: `http_log.c`.

Acceptance: colored on a TTY, plain with `NO_COLOR`, no escape codes to a file.

**Quality gate**
- [ ] Code quality (TTY/`NO_COLOR` decided once at sink build, not per record)
- [ ] No duplicated logic — **reuse the B2 attribute-iteration helper**; colors
      from one `static const` ANSI table, not inline literals per level
- [ ] `const` — ANSI table `static const`; level→color lookup const
- [ ] Comments reviewed
- [ ] Tests — color forced on/off; NO_COLOR honored; file path has no escapes
- [ ] Coverage checked
- [ ] Build & verify

### Stage B4 — `setLogSinks` config  _(step 9)_

Goal: `setLogSinks(array $sinks)` — declarative list of
`{type, target/path, format, level, …}`. `setLogSeverity`/`setLogStream` stay as
sugar for the single-file case (back-compat). Pool mode stamps `worker_id`.

Files: `http_server_config.c`, `stubs/HttpServerConfig.php`, sink construction
in `http_log_server_start`.

Acceptance: multiple sinks active at once, each with its own format + level.

**Quality gate**
- [ ] Code quality (validate sink specs at config time, fail loud)
- [ ] No duplicated logic — sugar setters **build a one-element sink array**
      through the same path as `setLogSinks`; no parallel single-sink code
- [ ] `const`
- [ ] Comments reviewed
- [ ] Tests — 3 sinks / 3 formats / distinct levels; invalid spec rejected
- [ ] Coverage checked
- [ ] Build & verify

### Stage B5 — external transports (incremental)  _(step 10 — ⚠ profiler)_

Goal: `file` (+ rotation by size/time), `stdout`/`stderr`, **syslog** (RFC
5424), **systemd journal** (`sd_journal`, structured), **TCP/UDP**. Each is a
`transport_write_fn`; reuse the async coalescing writer. Land file/console
first, add the rest incrementally — each transport is a sub-stage with its own
gate.

Files: `http_log.c` (+ optional build flag for journald).

Acceptance: per transport, records arrive intact under load; sink failure is
isolated (drop-counted, others unaffected).

**Quality gate (per transport)**
- [ ] Code quality (blocking syscalls kept off the reactor thread)
- [ ] No duplicated logic — transports differ only in `write_fn`; framing/
      pending/drain is **shared**, not re-implemented per transport
- [ ] `const`
- [ ] Comments reviewed
- [ ] Tests — per-transport integration test; failure-isolation test
- [ ] Coverage checked
- [ ] Build & verify
- [ ] **Profiler** — the sink write must not stall the reactor loop; measure
      reactor tick latency under sustained log volume per transport

### Stage B6 — structured access log  _(step 11 — ⚠ profiler)_

Goal: per-request event (method, path, status, bytes, duration, protocol,
remote addr, trace id) through the same pipeline, own category/severity so it
filters independently. Closes the "JSON access log" item of #5.

Files: request-completion path; `http_log.c` (category tag).

Acceptance: one line per request; routable to a different sink than diagnostics.

**Quality gate**
- [ ] Code quality (built from data already on the request — no re-parse)
- [ ] No duplicated logic — **reuse the attr/formatter pipeline**; access log is
      a record with a category, not a second logging path
- [ ] `const` — request read as `const` while building the event
- [ ] Comments reviewed
- [ ] Tests — access-log line across H1/H2/H3; category routing
- [ ] Coverage checked
- [ ] Build & verify
- [ ] **Profiler** — per-request RPS unchanged with access log on vs off

### Stage B7 — `onLog` PHP hook (+ userland OTLP)  _(step 12)_

Goal: `onLog(callable)` — record delivered to userland (sink type `php`). OTLP-
logs export is done in userland on top of `onLog`, **not** an embedded client.

Files: `http_server_class.c`, `stubs/HttpServer.php`.

Acceptance: userland callback receives structured records; exceptions in it are
absorbed, never kill the worker.

**Quality gate**
- [ ] Code quality (callback invoked safely; exception absorbed like other hooks)
- [ ] No duplicated logic — the `php` sink is another `transport_write_fn`, not a
      bypass around the fan-out
- [ ] `const`
- [ ] Comments reviewed
- [ ] Tests — userland callback receives records; exception absorbed
- [ ] Coverage checked
- [ ] Build & verify

---

## Relationship to issue #5

| #5 pillar | This plan |
|-----------|-----------|
| Metrics (counters/gauges) | Part A — `getTelemetry()` array; export is userland |
| Prometheus endpoint | **dropped** (no embedded server) |
| Structured logs / JSON access log | Part B (B2, B6) |
| Tracing / spans | trace context already ingested; spans later via `onSpan` — out of scope now |
| Zero-cost when off | counter bumps always-on & cheap; API+slab gated; log fast-gate short-circuits at severity OFF |

## Out of scope (for now)

- Embedded Prometheus / OTLP servers or clients in C.
- Duration histograms / percentiles (A6, deferred).
- Distributed-tracing span emission.
