# Static-Handler Branch — Compliance Review

Audit of `13-built-in-static-file-handler` (`src/static/`,
`include/static/`, +4402 lines across 11 files) against
`docs/CODING_STANDARDS.md` as updated in this session.

## Fix-pass status (in this branch)

Items marked **DONE** were addressed in the fix-pass; **DEFERRED**
items require larger-scope work (own PR / architectural decision)
and are listed in §13.

| Item | Status |
|---|---|
| `clang-format -i` on all 11 files (with `InsertBraces: true`) | **DONE** |
| 41 brace-less `if` bodies | **DONE** (formatter) |
| Missing blank lines between logical blocks | **DONE** (formatter) |
| `find_request_header` → `find_first_request_header` | **DONE** |
| `path_segments_owner_match` → `verify_path_owner_chain` | **DONE** |
| `PATH_MAX` → `MAXPATHLEN` | **DONE** |
| `find_extension_offset` IS_SLASH (mime) | **DONE** |
| `parse_http_date` post-strptime field validation | **DONE** |
| Path depth cap (`HTTP_STATIC_PATH_MAX_SEGMENTS=256`) | **DONE** |
| Defensive NULL → `ZEND_ASSERT` in cache | **DONE** |
| Strip inline `issue #13` / `PR #X` from function bodies | **DONE** |
| Post-open `dev`/`ino` re-check (§13d retrofit) | **DONE** |
| 32 cmocka unit tests for the 3 decoders + CMake wiring | **DONE — green** |
| Win32 port (timelib, fnmatch shim, php_sys_lstat, IS_SLASH segmenter) | **DEFERRED** |
| `openat`-chain TOCTOU rewrite | **DEFERRED** |
| Split `http_static.c` (2004 LoC) into 3 modules | **DEFERRED** |
| Type rename `http_static_handler_t` → `_mount_t` (or vice versa) | **DEFERRED** (decision) |

Severity legend: **B** = blocker (does not build / wrong on a target
platform / security gap), **H** = high (rule explicitly violated,
must fix), **M** = medium (cleanup before merge), **L** = low.

---

## 1. Cross-file blockers — §9.2 Cross-platform / Filesystem

| Item | Files | §  | Sev |
|---|---|---|---|
| `<fnmatch.h>` + libc `fnmatch()` | `http_static_path.c:16,231` | §9.2 | **B** |
| `strptime()` + `timegm()` | `http_static_etag.c:209,210,214,215,219,220` | §9.2 | **B** |
| Raw libc `lstat()` / `stat()` against bare `struct stat` | `http_static.c:141,204,234`, `http_static_etag.c:20–30` | §9.2 | **B** |
| Raw libc `realpath()` (rather than `tsrm_realpath` / `virtual_realpath`) | `http_static.c:1880` (resolved_under_root path) | §9.2 | **B** |
| `O_CLOEXEC` without `#ifdef` | `http_static.c:105,1435` | §9.2 | **B** |
| `PATH_MAX` instead of `MAXPATHLEN` | `http_static.c:153,179,233,1386,1641,1696`; `http_static_path.c:153` | §9.2 | **H** |
| Path-segment walk on `'/'` only (must use `IS_SLASH`) | `http_static.c` owner-walk; `http_static_mime.c:94`; `http_static_path.c:82–84` | §9.2 | **H** |
| `st_uid` owner-match logic without Win32 fallback policy | `http_static.c` path_segments_owner_match | §9.2 | **H** |
| `st_mtime` (seconds only) on `_WIN32` lowers ETag entropy without note | `http_static_etag.c:25–29` | §9.2 | **M** |
| `st_ino` used as ETag entropy without Win32 fallback | `http_static_etag.c:75` | §9.2 | **M** |

Until these are fixed the module is POSIX-only. A `#if !defined(_WIN32)`
gate on the whole module is acceptable as a transitional step *if*
documented in `static_handler.h` and CMake skips the module on win32.

---

## 2. Brace-less `if` bodies — §13a.2 (mandatory)

41 occurrences across 6 files; representative list:

- `static_handler_class.c:351, 357, 368, 372, 378` (and ≥3 more)
- `http_static.c:217, 263, 264, 472, 779, 786, 788, 798, 800, 809, 818, 1317, 1512, 1519, 1521, 1531, 1536, 1543, 1549, 1551, 1554, 1566, 1572, 1573, 1609, 1636` — 27 total
- `http_static_etag.c:190`
- `http_static_mime.c:94, 95, 110, 125, 126, 139`
- `http_static_cache.c:69`

Severity: **H** — `.clang-format` already enforces
`AllowShortIfStatementsOnASingleLine: false`; running clang-format on
the branch will rewrite these. The fact that they are still here means
the formatter has not been run, which is itself a process violation.

---

## 3. Missing blank lines — §13a.1

Confirmed regions (Explore-agent findings, retained):

- `static_handler_class.c:399–407, 410–417` — constructor body packs
  ZEND_PARSE → validation → assignment with no separators.
- `http_static.c:67–73, 1011–1023` — header lookups + conditional_match
  packed.
- `http_static_path.c` `percent_decode()` — reads as one wall of code
  with no internal pacing despite four distinct rejection sites.

Severity: **H** — readability rule is now codified in §13a.1 and the
PR is large enough that this materially slows review.

---

## 4. Function length — §13a.3

| Function | File | Lines | Sev |
|---|---|---|---|
| `http_static_try_serve` | `http_static.c:1660–1950` | ~290 | **H** |
| `ss_dispatch` | `http_static.c:766–840` | ~75 | M |
| `http_static_handler_freeze` | `static_handler_class.c:266–347` | ~80 | M |
| `ss_finalize` | `http_static.c:518–575` | ~55 | L |

`http_static.c` itself at 2004 lines is too large; it currently
contains the dispatch state machine, range parsing, precompressed
selection, header emission, and the open/stat path. Split candidates:
`http_static_serve.c` (state machine), `http_static_range.c` (range
header + If-Range), `http_static_negotiate.c` (Accept-Encoding +
sidecar selection).

Severity: **H** for `try_serve`, **M** for the file split.

---

## 5. Comments — §13a.4 / §4

Density (lines containing `/*`/`//`/`*/` over total):

| File | Comment lines | Total | Ratio |
|---|---|---|---|
| `http_static.c` | 221 | 2004 | 11% |
| `static_handler_class.c` | 42 | 996 | 4% |
| `http_static_cache.c` | 37 | 273 | 14% |
| `http_static_etag.c` | 27 | 248 | 11% |
| `http_static_path.c` | 19 | 236 | 8% |
| `http_static_mime.c` | 12 | 191 | 6% |

Density itself is fine. Specific issues:

- **Issue/PR refs in function bodies** — `http_static.c` references
  "issue #13 PR #X" 15+ times in inline comments (lines 9, 333, 349,
  441, 442, 1043, 1490, 1595…). §13a.4 allows these in module-header
  banners but not inline. **Sev: M**, batch-removable.
- **Informal TODO ref** — `http_static.c:144,145` mentions
  `TODO_STATIC_HANDLER_REVIEW #15`. Either link to a tracked issue or
  delete. **Sev: L**.
- **WHAT-not-WHY clusters** — `http_static_cache.c:35,40,45,47,52,53`
  restate variable names. **Sev: L**, delete.
- **Multi-paragraph helpers** — `http_static.c:107–116`,
  `http_static.c:333–348` are long but justified (security policy,
  async ownership chain). Keep.

---

## 6. Naming — §13b

### 6.1 Type / variable parity (§13b.1)

`http_static_handler_t` is used with parameter name `mount` in:

- `static_handler.h:193,194` — public API: `shared_addref(mount)`,
  `shared_release(mount)`.
- `http_static_path.h:44,58` — `path_resolve(... mount, ...)`,
  `path_is_hidden(... mount, ...)`.
- `http_static_mime.h:28` — `mime_lookup(... mount, ...)`.
- `http_static.c` and `static_handler_class.c` — both spellings
  (`mount`, `handler`, `m`, `h`) appear, sometimes in the same
  function.

Decision required: rename the **type** to `http_static_mount_t` (every
call site already calls it that conceptually) **or** rename every
parameter to `handler`. Mixing both is the current state and is
forbidden by §13b.1. **Sev: H**, mechanical.

### 6.2 Security predicate naming (§13b.2)

- `path_segments_owner_match()` — returns bool used as access gate.
  Name is neutral comparison, not decision verb. Rename to
  `verify_path_owner_chain` or `path_owner_chain_is_trusted`.
  **Sev: H**.

### 6.3 First-of-many returns (§13b.3)

- `find_request_header()` (`http_static.c:81–102`) — returns only
  the first value of a multi-valued header. Rename to
  `find_first_request_header`. If `Set-Cookie` / `Via` / `Forwarded`
  / repeated `Accept-Encoding` callers exist or will exist, also add
  `find_all_request_headers`. **Sev: H** — footgun for next
  developer.

### 6.4 Cryptic locals (§3, existing rule)

- `ss_*` prefix for static-serve state machine — documented at
  `http_static.c:333`, acceptable.
- Single-letter `n`, `k`, `v`, `p`, `q` — used in tight loops only,
  acceptable per §3 «Length».
- `m` and `h` for `http_static_handler_t` locals — not acceptable
  given §6.1 above. Pick one full name, use it.

---

## 7. Decoders — §13c

Three decoders parse user-controlled bytes:

| Decoder | File | Cmocka tests | §13c.4 |
|---|---|---|---|
| `percent_decode` | `http_static_path.c:24–73` | absent | **B** |
| `validate_segments` | `http_static_path.c:80–110` | absent | **B** |
| `parse_http_date` | `http_static_etag.c:198–223` | absent | **B** |
| `match_if_none_match` | `http_static_etag.c:163–195` | absent | **B** |
| `lookup_builtin` (MIME) | `http_static_mime.c:117–130` | absent | **H** |
| `parse_byte_range` | `http_static.c:1505–1577` (single-range) | absent | **B** |

§13c.4 was added to standards this session — none of the new
decoders ship with unit tests. Action item: scaffold
`tests/unit/static/test_path_decode.c`,
`test_path_segments.c`,
`test_etag_conditional.c`,
`test_mime_lookup.c`,
`test_byte_range.c` before merge.

### 7.1 Caps — §13c.1

- URL path / decoded-tail length: bounded only by `PATH_MAX` buffer
  (line 153). §13c.1 says buffer size is not a cap. **Sev: M** — add
  explicit max-length check up-front.
- Path depth (segment count): no cap. A request with thousands of
  short segments forces O(N) lstat-walk in OWNER mode → DoS.
  **Sev: H**.
- `If-None-Match` comma-list length / entry count: no cap. A 100KB
  `If-None-Match` header walks `match_if_none_match` byte-by-byte.
  Likely capped upstream by header-size limit, but not locally.
  **Sev: M** — verify upstream cap exists; if so, document it; if
  not, add one.
- Hide-glob count (`mount->hide_count`) loop with `fnmatch` per
  request. Operator-controlled, low risk, but still needs a cap.
  **Sev: L**.

### 7.2 Reject-not-normalize — §13c.2

`percent_decode` is correct: rejects `%00`, literal NUL, literal `\`,
`%5C`. **Compliant.**

`validate_segments` rejects `.`, `..`, empty segments. **Compliant.**

`parse_http_date` does not validate `struct tm` fields after
`strptime` returns success. POSIX does not specify whether `strptime`
rejects out-of-range fields (day=99, month=99, second=60). Add
post-parse range checks. **Sev: H**.

### 7.3 Don't reinvent without checking PHP-core — §13c.3

Already done where unsafe (`php_url_decode` correctly rejected:
accepts `%00` / `\\`).

Missed:

- `strptime` + `timegm` should be replaced by **timelib** or a
  module-local fallback. Current code does not link on MSVC.
  **Sev: B**.
- `fnmatch.h` — no portable PHP wrapper; needs ifdef + fallback.
  **Sev: B**.

---

## 8. TOCTOU / filesystem access — §13d

Current model in `http_static.c`:

1. `path_segments_owner_match` does an `lstat`-walk over each segment
   (intermediate dirs and final file).
2. `http_static_path_resolve` builds the canonical FS path string.
3. `open(path, O_RDONLY | O_CLOEXEC [| O_NOFOLLOW])` — plain `open`
   on the assembled path.
4. After open, `resolved_under_root()` runs `realpath` and compares
   against the mount root.

§13d forbids `lstat`-walk + plain `open()`. The realpath check at (4)
is correctly present *as a supplement* but is not sufficient on its
own under §13d (it catches escape, but not all swap variants between
walk and open).

Required: switch to either
- `openat2(root_fd, RESOLVE_NO_SYMLINKS|RESOLVE_BENEATH)` on Linux ≥5.6,
- per-segment `openat(O_NOFOLLOW)` chain from a startup-pinned root
  fd,
- or a post-open `fstat` + path-`stat` `dev`/`ino` re-check (the
  cheapest retrofit).

**Sev: H**. The current design is "documented insecure but mitigated";
§13d says that is no longer acceptable.

Other §13d items:

- O_NOFOLLOW only on REJECT mode by design; OWNER mode is gated by
  the `lstat`-walk which is exactly the pattern §13d forbids. So
  OWNER mode needs the openat-chain rewrite even more urgently than
  REJECT mode.
- `IS_SLASH`-vs-`'/'` segmentation is also a pre-condition: on a
  hypothetical Windows port, `\\` smuggles past the walk. URL decoder
  rejects literal `\`, which closes the inbound vector — but the
  internal segmenter must still match the kernel's parsing. Note in
  the file header.

---

## 9. Defensive checks — §13e

### 9.1 Internal helpers with dead NULL checks

- `http_static.c:472` `if (state == NULL) return;` in `_free`-shape
  function — acceptable per §13e *if* the contract is documented.
  Add a one-line "NULL is a no-op" comment or convert to
  `ZEND_ASSERT`. **Sev: L**.
- `http_static.c:1317` similar.
- `static_handler_class.c:351,357` `if (mount == NULL) return;` —
  same shape.
- `http_static_mime.c` `mime_lookup` — `mount != NULL` check is
  load-bearing because the function is also called from a fast-path
  with no per-mount overrides. Acceptable.
- `http_static_cache.c` `lookup` / `insert` — defensive NULL checks
  on `cache`, `path`, `out_view`. Convert to `ZEND_ASSERT` (§13e
  rule 3) — these are TU-internal callers with a known contract.
  **Sev: M**.

### 9.2 First-arg type-narrowing checks

`http_static_handler_from_obj()` (`http_static.c:47–52`) — checks
`obj->ce != http_static_handler_ce`. This is a system-boundary check
(crosses user → core), **keep**.

---

## 10. Per-file summary

### `static_handler.h` (233 lines)
- §13b.1: type/var parity — `mount` parameters on public API. **H**.
- Acceptable header-banner comments.

### `http_static_cache.h/c` (82 + 273)
- §13a.2 brace-less if (line 69). **H** mechanical.
- §13a.4 "WHAT" comments. **L**.
- §13e: convert defensive NULL → ZEND_ASSERT. **M**.

### `http_static_etag.h/c` (46 + 248)
- §9.2 strptime/timegm — **B**.
- §13c.2 missing post-strptime tm validation — **H**.
- §13c.4 no unit tests — **B**.
- §9.2 Win32 mtime/ino entropy notes — **M**.

### `http_static_mime.h/c` (32 + 191)
- §13b.1 `mount` parameter naming — **H**.
- §9.2 `find_extension_offset` only checks `'/'`, not `\` — **H**.
- §13a.2 brace-less if cluster — **H** mechanical.
- §13c.4 no unit tests — **H**.
- §13c.3: align built-in MIME table with `sapi/cli/php_cli_server.c`
  prior art; document divergence. **L**.

### `http_static_path.h/c` (61 + 236)
- §9.2 `<fnmatch.h>` + `fnmatch()` — **B**.
- §9.2 `PATH_MAX` — **H**.
- §13b.1 `mount` parameter naming — **H**.
- §13c.4 no unit tests for `percent_decode` / `validate_segments` —
  **B**.
- §13c.1 missing depth/length caps — **H**.
- §13a.1 missing blank lines in `percent_decode` — **H**.
- `percent_decode` itself is **compliant** with §13c.2 (correctly
  rejects ambiguous bytes).

### `http_static.c` (2004 lines)
- §13a.3 file size split — **M**.
- §13a.3 `try_serve` length — **H**.
- §13a.2 brace-less if (27 instances) — **H**.
- §13a.4 inline issue/PR refs (15+) — **M**.
- §13b.2 `path_segments_owner_match` rename — **H**.
- §13b.3 `find_request_header` rename + add `_first_` — **H**.
- §13c.1 path depth + range header caps — **H**.
- §13c.4 no tests for `parse_byte_range` — **B**.
- §13d TOCTOU on lstat-walk + open — **H**.
- §9.2 lstat/realpath/PATH_MAX/O_CLOEXEC — **B/H**.

### `static_handler_class.c` (996 lines)
- §13a.3 acceptable; consider extracting `freeze` helpers. **L**.
- §13a.2 brace-less if (≥5) — **H**.
- §13a.1 packed constructor body — **H**.
- §13b.1 mixed `mount` / `handler` / `m` — **H**.

---

## 13. Deferred — separate PRs

These items did not land in the fix-pass and require their own PR /
design decision:

### 13.1 Win32 port — large

- `<fnmatch.h>` + `fnmatch()` — vendor a minimal port or skip
  hide-glob feature on Win32.
- `strptime` + `timegm` — port to **timelib** (`timelib_strtotime`)
  for cross-platform HTTP-date parsing.
- Raw `lstat()` / `stat()` → `php_sys_lstat` / `VCWD_STAT` with
  `zend_stat_t`.
- Raw libc `realpath()` → `tsrm_realpath` / `virtual_realpath`.
- `O_CLOEXEC` and `O_NOFOLLOW` behind `#ifdef`.
- Path-separator handling in `validate_segments` and the OWNER walk —
  use `IS_SLASH(c)` (the mime decoder is already fixed via
  `is_path_separator`).
- POSIX `st_uid` / `st_gid` ownership — OWNER mode needs a Win32
  fallback policy or `#if !defined(_WIN32)` gate.
- `PHP_WIN32` build of the test target with stubbed `<fnmatch.h>`.

### 13.2 TOCTOU openat-chain rewrite — architectural

The post-open `dev`/`ino` re-check landed in the fix-pass closes the
common swap variant cheaply. The full §13d-compliant solution is:

- Pin a `root_fd` at mount setup
  (`open(root_directory, O_DIRECTORY|O_NOFOLLOW)`).
- Replace `lstat`-walk + `open(path)` with `openat2(root_fd, …,
  RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS)` on Linux ≥ 5.6, falling
  back to a per-segment `openat(O_NOFOLLOW)` chain.
- Drop `path_segments_owner_match` (`verify_path_owner_chain`)
  entirely once the kernel does the work.

### 13.3 Module split — `http_static.c` 2004 LoC → 3 modules

- `http_static_serve.c` — dispatch state machine (`ss_*`).
- `http_static_range.c` — Range / If-Range parsing.
- `http_static_negotiate.c` — Accept-Encoding + sidecar selection.

### 13.4 Type / variable parity decision

`http_static_handler_t` parameters are still spelled `mount` in
public headers and most call sites. Pick one:

- (a) Rename the **type** to `http_static_mount_t` — every call site
  already calls it `mount` conceptually.
- (b) Rename every parameter to `handler` — keeps the type name
  but is more touch.

Either is mechanical once the decision is made; deferred so the
fix-pass diff stays reviewable.

---

## 11. Action plan (suggested order)

1. **Mechanical first pass — zero design risk:**
   - `clang-format -i` on every changed file (kills §13a.2 + most
     §13a.1 in one shot).
   - Global rename: pick `http_static_mount_t` (rename type) **or**
     rename every `mount` parameter to `handler`.
   - Replace `PATH_MAX` → `MAXPATHLEN`.
   - Strip inline issue/PR refs from function bodies; keep only in
     module-header banner.

2. **Naming — small but visible:**
   - `path_segments_owner_match` → `verify_path_owner_chain`.
   - `find_request_header` → `find_first_request_header` (+ optionally
     `find_all_request_headers`).

3. **Decoder unit tests — §13c.4 is mandatory before merge:**
   - `tests/unit/static/test_path_decode.c`
   - `tests/unit/static/test_path_segments.c`
   - `tests/unit/static/test_etag_conditional.c`
   - `tests/unit/static/test_mime_lookup.c`
   - `tests/unit/static/test_byte_range.c`
   - Cover the per-decoder kill-list from §13c.4.

4. **Win32 portability — design decision required:**
   - Either gate the whole module `#if !defined(_WIN32)` with a CMake
     skip and a documented limitation,
   - or port: timelib for HTTP-date, IS_SLASH segmenter, MAXPATHLEN,
     `php_sys_lstat`, `tsrm_realpath`, fnmatch shim.

5. **TOCTOU rewrite — §13d:**
   - Open root_fd at mount setup.
   - Replace `lstat`-walk + `open(path)` with openat-chain
     (portable) or `openat2(RESOLVE_BENEATH)` (Linux fast path).
   - Keep `resolved_under_root` as a supplemental check.

6. **Defensive cleanup:**
   - Convert internal NULL guards to `ZEND_ASSERT` per §13e rule 3.
   - Document `_free`-shape NULL-as-no-op contracts inline.

7. **Caps — §13c.1:**
   - Path depth cap up-front in `validate_segments`.
   - Verify upstream header-size cap covers `If-None-Match`; document.

---

## 12. What is *compliant*

Not everything in this branch is broken. For credit:

- §3 prefix-by-subsystem (`http_static_*`) is consistently applied.
- §6 const correctness is well-honoured (locals are `const T *const`
  where possible, struct cache pointers are const).
- §2 `EXPECTED` / `UNEXPECTED` placement is mostly correct on hot
  paths.
- `percent_decode` rejection set is correct and stricter than
  `php_url_decode` (§13c.2).
- `match_if_none_match` correctly implements RFC 9110 §13.1.2
  weak-equal comparison.
- Header banners + design docs (`docs/PLAN_STATIC_HANDLER.md`)
  capture the WHY for non-obvious choices (hand-rolled date format,
  XOR-mix ETag).
- Bsearch + sort-asserted MIME table is the canonical PHP-core
  pattern (`sapi/cli/php_cli_server.c` does the same).
