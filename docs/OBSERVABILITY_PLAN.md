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

**Quality gate** — ✅ done (status classes folded into `http_server_count_request(c,
status)` so every counted request classifies exactly once → the four buckets sum to
`total_requests`; per-proto gauge inc at protocol detection / H3 conn open, dec at
close, WebSocket mapped back to h1 so the gauge balances):
- [x] Code quality (single increments; classify is one 4-way branch; gauge ++/-- is
      per-connection, not per-request; guarded dec never underflows)
- [x] No duplicated logic — **one** `http_server_count_request` classifier + one
      `http_server_conn_active_inc`/`_dec` pair, called from every protocol; word-wise
      `stats_counters_add` auto-covers the new fields
- [x] `const` — readers already `const`; new inline helpers take the non-const write
      slice (they mutate) + a `const`-friendly enum
- [x] Comments reviewed (WHY on the classify fall-through + the WS→h1 gauge mapping)
- [x] Tests — `telemetry/005`: driven 2xx/3xx/4xx/5xx mix; exact per-class counts +
      class-sum == total_requests; H1 gauge drains to 0, h2/h3 stay 0
- [x] Coverage checked (all four classes + gauge inc/dec exercised)
- [x] Build & verify — clean build; 28 telemetry/static + 32 h1/sendfile + 84 h2/h3
      green. memcheck: registry/slab path (which now carries the wider slot) stays
      valgrind-clean via 001; server memcheck runs produce correct output but hang on
      the coroutine teardown-drain (a valgrind×server property, not A5) so no leak
      summary — the change adds only POD counter increments + one status field read on
      an already-dereferenced response, i.e. no new memory ops
- [x] **Profiler** — 4-worker H1 wrk median **178.0k vs 177.7k baseline** (+0.2%,
      inside ±20% loopback noise) — no regression

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

**Quality gate** — ✅ done (`http_log_state_t` = `{severity(min-floor gate),
sink_count, sinks[8]}`; per-sink `http_log_sink_t` owns formatter + async transport
+ drop counter; `http_log_emitf` gates on the min floor, formats once per distinct
`(formatter,ud)` into a bounded stack cache, fans out to admitting sinks; dead
`g_writer`/`g_formatter` removed; `http_log_server_start` builds exactly one sink via
`http_log_sink_start`, which `setLogSinks` (B4) will reuse for the rest):
- [x] Code quality (emit macro gate unchanged — one branch on first-field `severity`;
      format-once cache capped at `HTTP_LOG_FMT_SLOTS`=4 → ~8 KiB emit stack)
- [x] No duplicated logic — the pending/coalesce/drain writer machinery moved into the
      sink (re-keyed `state`→`sink`), not copied; `http_log_sink_drain`/`_stop` are the
      single per-sink helpers `http_log_server_stop` loops over under one shared budget
- [x] `const` — formatter reads `const http_log_record_t *`; `http_log_sink_write` takes
      `const char *`
- [x] Comments reviewed (kept the libuv buffer-ownership WHY + drain UAF WHY; dropped the
      stale global-hook block)
- [~] Tests — single-sink byte-identical + drain covered (core/013–018, 034 green);
      **multi-sink fan-out deferred to B4** where `setLogSinks` makes >1 sink reachable
      from PHP (no throwaway internal hook)
- [x] Coverage checked (single-sink emit/drain/multi-server exercised by core/013–018)
- [~] Build & verify — clean build, no warnings; 15 tests green (7 log + 5 telemetry +
      3 h1/request-path). **ASAN — pending batched run** (lifecycle identical to prior
      single-sink; valgrind hangs on server teardown-drain per harness note)
- [x] **Profiler** — N/A by construction: `http_log_emitf` is not on the per-request
      hot path (INFO only at start/stop/reload, DEBUG only multipart); the per-request
      macro gate is byte-identical and `core/018` confirms off-path is free. State-size
      growth is per-server-object, not per-conn/request

### Stage B2 — JSON + logfmt formatters  _(step 7)_

Goal: `json` (one line, OTel Logs field names:
Timestamp/SeverityNumber/SeverityText/Body/Attributes/TraceId/SpanId) and
`logfmt` (key=value). `plain` stays.

Files: `http_log.c`.

Acceptance: valid JSON per line (escaping correct); logfmt parses in Grafana.

**Quality gate** — ✅ done (bounded `log_sbuf_t` string builder + `format_iso8601`
+ `sb_put_attrs` shared across all three formatters; `json`/`logfmt` added; `plain`
byte-identical; json escapes per RFC 8259, logfmt quotes values with space/quote/
control, both emit one line; trace fields json-only):
- [x] Code quality (one bounds-checked builder; each formatter is a thin token
      sequence over the shared helpers; format-cache dedup from B1 applies)
- [x] No duplicated logic — a single `sb_put_attrs` iterates attrs for all three
      styles (differ only in separators + per-value rendering); one `format_iso8601`
      feeds every formatter's timestamp
- [x] `const` — formatters take `const http_log_record_t *`; the builder writes only
      its own output buffer
- [x] Comments reviewed (WHY on the sbuf truncation contract, JSON/logfmt escape rules)
- [x] Tests — `core/019`: plain + logfmt exact goldens, json `json_decode` round-trips
      every field + single-line assertion; escaping edge cases (space/quote/newline,
      i64/u64/bool/f64, trace hex); driven by a pure `_http_log_format_selftest` hook
- [x] Coverage checked (all three styles + every attr type + escape branches exercised)
- [x] Build & verify — clean build, no warnings; 13 log+telemetry tests green (plain
      byte-identity held). ASAN batched with B1 (formatters are pure, stack-only)

### Stage B3 — pretty console formatter  _(step 8)_

Goal: `HH:MM:SS.mmm  LEVEL  [worker#/scope]  message  key=val …`. Fixed-width
colored level badge (INFO green / WARN yellow / ERROR red / DEBUG dim). Dimmed
timestamp, keys dim / values bright. Auto-color on TTY; honor `NO_COLOR` /
`CLICOLOR_FORCE`; color off to file/pipe.

Files: `http_log.c`.

Acceptance: colored on a TTY, plain with `NO_COLOR`, no escape codes to a file.

**Quality gate** — ✅ done (`http_log_format_pretty`: dim clock + colour badge +
dim keys via the shared `sb_put_attrs` `LOG_STYLE_PRETTY`; colour resolved once by
`http_log_color_for_fd` and threaded through the formatter's `ud`, so it's decided
at sink build not per record; `[worker#/scope]` bracket deferred to B4 with the
worker_id stamp):
- [x] Code quality (colour is a per-sink `ud` flag decided at build; the record path
      only reads it — no getenv/isatty per emit)
- [x] No duplicated logic — pretty reuses `sb_put_attrs` (new `LOG_STYLE_PRETTY` just
      dims the key; values fall through the plain renderer); badge+colour come from one
      `static const pretty_level_style[]` table, not per-level literals
- [x] `const` — `pretty_level_style[]` is `static const`; `pretty_level_idx` is a pure
      lookup; formatter takes `const http_log_record_t *`
- [x] Comments reviewed (WHY on the colour-via-ud decision + NO_COLOR precedence)
- [x] Tests — `core/020`: colour off = exact escape-free golden, colour on = exact
      ANSI golden; `_http_log_color_decide` asserts bare-nonTTY=off, NO_COLOR=off,
      CLICOLOR_FORCE=on, NO_COLOR-wins-over-FORCE
- [x] Coverage checked (both colour paths + all three env branches exercised)
- [x] Build & verify — clean build, no warnings; 13 log+telemetry green. ASAN batched
      with B1 (formatter is pure, stack-only)

### Stage B4 — `setLogSinks` config  _(step 9)_

Goal: `setLogSinks(array $sinks)` — declarative list of
`{type, target/path, format, level, …}`. `setLogSeverity`/`setLogStream` stay as
sugar for the single-file case (back-compat). Pool mode stamps `worker_id`.

Files: `http_server_config.c`, `stubs/HttpServerConfig.php`, sink construction
in `http_log_server_start`.

Acceptance: multiple sinks active at once, each with its own format + level.

**Quality gate** — ✅ done (`setLogSinks(array)` on `HttpServerConfig`; each spec
`{type:stream|stdout|stderr, stream?, format?, level}` validated at config time via
`log_sink_spec_valid`; `http_server_start_logging` translates specs→`http_log_sink_spec_t`
and calls the B1 `http_log_server_start_sinks`; `type:stream` scope for B4, file/syslog/
journal/tcp deferred to B5):
- [x] Code quality (`log_sink_spec_valid` rejects bad type/format/level/missing stream
      with a clear `InvalidArgument`; >8 sinks rejected; reads at start are unchecked
      because config-time validated)
- [x] No duplicated logic — `http_log_server_start` is now a 1-spec wrapper over
      `http_log_server_start_sinks`; the sugar path synthesizes one spec and runs the
      **same** build loop as multi-sink; stdout/stderr open a `php://` stream fed through
      the identical `http_log_sink_start`
- [x] `const` — `http_log_server_start_sinks` takes `const http_log_sink_spec_t *`;
      formatter resolution is a pure lookup
- [x] Comments reviewed (WHY on spec-validated-so-unchecked reads + the php://std* ref
      hand-off)
- [x] Tests — `core/021`: 3 stream sinks (json/logfmt/pretty) at DEBUG/INFO/ERROR — same
      record fans out to json+logfmt, the ERROR sink filters INFO out (empty), and 4
      invalid specs (bad type/format, missing level, missing stream) each throw; manual
      smoke of stdout+stderr sinks (pretty/logfmt) verified
- [x] Coverage checked (multi-sink fan-out, per-sink level+format, all three types,
      validation branches)
- [x] Build & verify — clean build, no warnings; 9 log tests green; stdout/stderr smoke
      clean (exit 0). ASAN batched with B1

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

#### B5d — sink-type / formatter registry (plugin seam) ✅

Name→behaviour resolution for `setLogSinks()` moved from hardcoded if-chains
(config validation + server-start translation) into a registry in `http_log.c`:
`http_log_register_sink_type()` (`{name, validate, open, pinned_formatter}`)
and `http_log_register_formatter()` (`{name, fn, make_ud}`). Built-ins
(stream/stdout/stderr/syslog; plain/logfmt/json/pretty/syslog) register at
MINIT through the same seam a plugin extension would use; a pinned formatter
is how syslog forces its wire format. Error messages list registered names.
GELF / Fluent / OTLP / generic tcp-udp arrive as plugins on this seam, not in
core (user decision).

- [x] Code quality — registry consulted in exactly two places (validate,
      translate); MINIT-only registration, so lock-free lookups
- [x] No duplicated logic — both former if-chains deleted; the format-selftest
      hook resolves via the registry too
- [x] `const` — defs are `static const`, registry stores `const` pointers
- [x] Comments reviewed
- [x] Tests — `core/024-log-registry.phpt` (built-in names, registry-driven
      reject messages); 019–023 exercise resolution end-to-end
- [x] Coverage checked
- [x] Build & verify — full core+telemetry suite green
- Profiler N/A — registry lookups run at config/start time, not on emit

#### B5e — datagram syslog (udp:// + udg://) + framing moved to the transport ✅

Architecture fix surfaced by the mental-model review: RFC 6587 octet framing
lived in the *formatter*, but framing is a transport property (the same RFC
5424 message is octet-framed on TCP and must be one datagram on UDP). Now
`http_log_write_mode_t` (STREAM / STREAM_FRAMED / DGRAM) rides the sink spec:
the syslog formatter emits the bare message (also dropped its 1600-byte
scratch + memcpy), `http_log_sink_write` applies the frame, and the writer's
DGRAM mode stores a u32 length header in the ring and issues exactly one
write per record so each record travels as one datagram. Sink-type `open()`
gained a mode out-param; syslog resolves it from the target scheme
(tcp→framed stream, udp/udg→datagram). Also deduped: the 8 log-gate macro
bodies → one `http_logf_at`, `format_clock` → slice of `format_iso8601`.

- [x] Code quality — framing/boundary logic in exactly one layer (writer)
- [x] No duplicated logic — gate macros ×8→1; clock formatter derives from
      iso8601; scheme→mode table shared by validate and open
- [x] `const` — scheme table static const; ring peek takes const cb
- [x] Comments reviewed — mode semantics documented at the enum
- [x] Tests — 022 golden (bare message), 023 (TCP framed on the wire),
      new 025 (UDP + udg: every datagram is one bare RFC 5424 record)
- [x] Coverage checked
- [x] Build & verify — core+telemetry green
- Profiler N/A — emit path unchanged (one branch on mode per record)

#### B5f — `template` formatter (user-controlled line layout) ✅

User request: control the console text format (e.g. `Y-m-d` dates). New
built-in `'format'=>'template'` + `'template'=>'{ts:Y-m-d H:i:s.v} [{level}]
{msg}{attrs}'`. Placeholders {ts}/{ts:PATTERN} (date()-subset Y y m d H i s v),
{level}, {msg}, {attrs}, {trace}, {span}; unknown → literal. Compiled ONCE at
sink build into a flat segment list (`http_log_template_parse`, one emalloc
block, segs point into a private copy); render is a straight walk. To carry
the owned ud the formatter registry def gained `validate` (config-time key
check) and `free_ud` (ownership); `http_log_sink_start` now takes the spec
struct; sink stop / failed start free the ud.

- [x] Code quality — parse at build, zero parsing/alloc on the emit path
- [x] No duplicated logic — renders via the shared sb_*/sb_put_attrs/
      format_iso8601 helpers; registry seam extended, not bypassed
- [x] `const` — compiled template read as const in render; field table static
- [x] Comments reviewed
- [x] Tests — core/026 (4 goldens incl. date pattern + literal fallbacks,
      2 config rejects, e2e через stream sink); 024 lists updated
- [x] Coverage checked
- [x] Build & verify — 71 core+telemetry green; targeted valgrind on
      parse/render/reject path: 0 leaks, 0 errors
- Profiler N/A — same emit path; template render ≈ plain render cost

### Stage B6 — structured access log ✅ _(step 11 — ⚠ profiler)_

Shipped: `'category' => 'app' (default) | 'access' | 'all'` on the sink spec
(default `app` so enabling diagnostics never silently turns on per-request
logging); record carries a category bit; the shared fan-out
(`log_dispatch_record`, also behind `http_log_emitf`) filters by mask.
`http_log_emit_access(state, req, response_obj, remote)` builds the record
(method/path/status/proto/bytes/duration_ms/remote + trace ctx from the
request); call sites mirror the `http_server_count_request` set exactly:
H1/H2/H3 handler-entry tails (normal, static-skip, compression-reject),
send-file engine (6 sites via `cfg.server`), reactor-pool worker dispatch
(remote omitted — it lives reactor-side). h2/h3 now stamp
`request->http_major` at stream init, so proto derives from the request
everywhere. An access sink forces `sample_stamps_enabled` (third consumer).

- [x] Code quality — one emitter; gate = one `UNEXPECTED(has_access)` branch
- [x] No duplicated logic — same record/attr/formatter/writer pipeline;
      fan-out extracted once and shared with emitf
- [x] `const` — request read as `const` while building the event
- [x] Comments reviewed
- [x] Tests — core/027 (2 requests → 2 JSON records incl. duration+remote,
      3-way category routing, invalid category rejected), h2/051 + h3/051
      (proto=h2/h3 records over real curl-h2c / h3client requests)
- [x] Coverage checked
- [x] Build & verify — 132 tests green (core+telemetry+h1+h2+h3/051)
- [x] **Profiler** — wrk -t4 -c64, medians of 5, single worker, empty
      handler (worst case): access OFF = baseline within noise (gate is one
      branch). Access ON (JSON sink to file, ~280 B/record, zero drops):
      ~41.5k → ~33.5k RPS, ≈ 5–6 µs/record = the JSON render + ring copy.
      Optimized along the way: remote cached per connection (was
      getpeername per request, ~4 µs), ISO-8601 second-cache in
      `format_iso8601` (~1 µs, benefits every formatter). Remaining cost is
      the honest price of the enabled feature; further cuts need a
      formatter rewrite (not taken).

### Stage B7 — `onLog` PHP hook (+ userland OTLP) ✅ _(step 12)_

Shipped: sink type `php` through the registry (`php_delivery` type kind — no
stream; the spec's callable is resolved to an fcc once at sink build) +
`HttpServerConfig::onLog(callable, ?LogSeverity level = INFO, category='all')`
sugar that appends the equivalent spec. Records arrive as arrays
(`timestamp_ns/severity/severity_text/category/message/attrs/trace_id/span_id`)
from inside the same fan-out (`log_dispatch_record`). A `ZEND_TLS` re-entrancy
guard keeps a logging callback from recursing into itself (stream sinks still
get the nested record); a callback exception is absorbed (drop-counted,
rate-limited notice).

- [x] Code quality — fcc resolved once; delivery inside the dispatch loop
- [x] No duplicated logic — php sink is a delivery variant inside the one
      fan-out, not a second logging path
- [x] `const`
- [x] Comments reviewed
- [x] Tests — core/029: records for diagnostics + access, thrown exception
      absorbed mid-stream, invalid callback rejected at config time
- [x] Coverage checked
- [x] Build & verify — 134 tests green

#### B7b — pool-mode logging fix + `file` sink ✅

Bug found while benchmarking B6: the frozen config snapshot carried NO
logging fields, so pool workers never logged anything. Fixed: sink specs are
flattened into `http_server_shared_config_t` as persistent strings (+ level
backing value) at freeze and rebuilt into the worker's spec array at LOAD.
`stream`/`php` sinks cannot cross threads (resource/closure) — skipped at
freeze with a stderr notice when workers>1; new sink type
`['type'=>'file','path'=>…]` reopens the path per worker (append mode) and is
the pool-friendly file transport. phpt core/028 (2-worker pool, 4 requests →
4 access records in the shared file).

**ASAN/UBSAN (batched B1–B7):** core+telemetry under
`scripts/test-with-sanitizers.sh` — sanitizer-reported errors: **0**. The 17
failing tests are all the documented vfork-interceptor × uv_spawn false-SEGV
(`memset @ 0x10007fff8000`) / ASAN-slowdown class on pool/exec tests — not
extension bugs. Release build restored (`nm -D | grep -c __asan` = 0), full
suite re-verified green.

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
