# Coding Standards

Pure code-level rules. How the source must be written, regardless of who is
writing it. Process and workflow guidance lives in
[`RECOMMENDATIONS.md`](RECOMMENDATIONS.md).

---

## 1. Threading model

### 1.1 One server instance == one thread

Each `HttpServer` instance is **strictly single-threaded** by design. The
event loop, parsers, connection state machines, request/response objects,
counters and per-server caches all run on a single OS thread. This is the
basis for the lock-free hot path: plain reads, no atomics, no fences.

This is not a TODO or a limitation — it is a deliberate invariant. Do not
introduce locks, atomics, or "thread-safe" wrappers in core paths.

### 1.2 Multi-instance, multi-thread is real

Several `HttpServer` instances **can run concurrently** in different threads
of the same process (transfer-object via `ThreadChannel` is a supported
production scenario).

Consequences for code:

- Per-server runtime state lives **inside the `HttpServer` struct**, not in
  plain `static` globals. A `static` global shared between concurrent
  server instances is a race by construction.
- The "currently active server on this thread" must be stored in
  **thread-local storage** (`__thread` on GCC/Clang, `__declspec(thread)`
  on MSVC). See `current_server`, `http_log_active`.
- Hooks contributed by C extensions (writers, formatters, registered
  external globals) are **process-wide** and must be installed once at
  `MINIT`. After `MINIT` they are read-only.
- Cross-thread aggregation (per-thread counter rolled up into a
  process-wide metric) is done **above** the core. The core stays
  per-thread.

### 1.3 Thread-per-core / share-nothing pipeline

Each worker owns the full `accept` → TLS → parse → handler → encode →
`send` pipeline end-to-end on one core. Pipelining stages across cores is
forbidden: it loses to share-nothing on modern hardware (cache-coherency
traffic, USL β > 0). The only acceptable off-load is the **TLS handshake**
(asymmetric crypto), because handshakes are rare enough that the
cross-core transfer cost is amortised.

---

## 2. Branch prediction: `EXPECTED` / `UNEXPECTED`

On hot paths use Zend's `EXPECTED(cond)` / `UNEXPECTED(cond)` macros. They
expand to `__builtin_expect()` and improve i-cache layout plus reduce the
cost of mispredicting rare error branches.

Apply them to:

- Allocation NULL-checks: `if (UNEXPECTED(buf == NULL)) { ... }`
- Parse-error / validation-failure branches:
  `if (UNEXPECTED(content_length > max_body_size)) return ERR;`
- Success-path conditions: `if (EXPECTED(parser->state == STATE_NORMAL)) ...`
- Every parser hot loop, every per-frame / per-packet path, every
  per-request entry point.

Do **not** apply on cold paths (one-shot config init, error formatting,
shutdown, MINIT/MSHUTDOWN) — adds noise without benefit.

---

## 3. Naming

Names must be **descriptive, full English words**. Code is read far more
often than it is written; a one-character saving in typing is paid back
many times in reading cost.

### Forbidden

- Single-letter variables outside very short, conventional scopes. The
  only acceptable uses are: loop indices `i`, `j`, `k` in a tight numeric
  loop; pointer aliases `p` / `q` in a 2–3-line block; coordinate `x` /
  `y`. Anything that lives longer than ~5 lines or crosses a function
  boundary needs a real name.
- Cryptic abbreviations (`ntill`, `nbuf`, `cnxctx`, `hdrtbl`). If the
  reader has to guess what the consonant cluster stood for, the name is
  wrong. Use `until_byte`, `buf_len`, `connection_ctx`, `header_table`.
- "Hungarian-style" type prefixes (`pBuf`, `iCount`, `szName`). The type
  is in the declaration; do not duplicate it in the name.
- Numeric suffixes for "another one of these" (`buf2`, `tmp3`,
  `handler_new`). Pick a name that says what makes this one different
  (`scratch_buf`, `pending_buf`, `inflight_handler`).
- Negated booleans (`not_ready`, `disable_no_keepalive`). Use the
  positive form (`ready`, `keepalive_enabled`); `if (!ready)` reads
  cleanly.

### Required

- Local variables: `lower_snake_case`, full words.
  `connection_count`, not `cc` / `cnt` / `n_conn`.
- Functions: `lower_snake_case`, prefixed by the subsystem they live in
  — `http2_session_recv_frame`, `http3_stream_close`,
  `tls_handshake_continue`. The prefix is what makes grep useful.
- Struct types: `lower_snake_case_t` matching the subsystem prefix
  (`http2_session_t`, `http3_stream_t`).
- Macros and compile-time constants: `UPPER_SNAKE_CASE`.
- Booleans read as predicates: `is_*`, `has_*`, `should_*`, `*_enabled`.
- Counts and sizes: `*_count`, `*_len`, `*_bytes`. Do not mix these —
  `len` is in items of the container's element type, `bytes` is always
  in bytes.
- Acronyms that appear in identifiers stay lower-case in
  `lower_snake_case` (`http2_id`, not `HTTP2_id`; `tls_ctx`, not
  `TLSctx`).

### Length

A name should be exactly long enough to be unambiguous in its scope. A
two-line block can use `p`. A 30-line function with three different
buffers needs `header_buf`, `body_buf`, `trailer_buf`. When in doubt,
prefer one extra word over one missing word.

---

## 4. Comments

Default: **no comment**. Add one only when removing it would lose
non-obvious WHY: a hidden invariant, a workaround for a specific bug, an
edge case that the reader cannot derive from the code and the identifier
names.

Do not write:

- Decorative comments restating WHAT the next line does.
- Multi-paragraph docstrings on internal helpers.
- References to the current task / PR / fix ("added for the upload flow",
  "fixes #123") — those belong in the commit message.
- "Used by X" call-site notes — they rot the moment the caller moves.

Before writing a comment, ask: *if I delete this line, does a future reader
lose information they cannot recover from the code?* If no — do not write
it.

---

## 5. Early return / guard clauses

Prefer early returns over nested `if`. A function should validate its
preconditions and bail out at the top, leaving the main logic flat at one
indentation level.

Bad:

```c
static int handle_frame(http2_session_t *session, http2_frame_t *frame) {
    if (session != NULL) {
        if (frame != NULL) {
            if (session->state == H2_STATE_OPEN) {
                /* ... real work, four levels deep ... */
                return process(session, frame);
            } else {
                return ERR_BAD_STATE;
            }
        } else {
            return ERR_NULL_FRAME;
        }
    } else {
        return ERR_NULL_SESSION;
    }
}
```

Good:

```c
static int handle_frame(http2_session_t *session, http2_frame_t *frame) {
    if (UNEXPECTED(session == NULL)) return ERR_NULL_SESSION;
    if (UNEXPECTED(frame == NULL))   return ERR_NULL_FRAME;
    if (UNEXPECTED(session->state != H2_STATE_OPEN)) return ERR_BAD_STATE;

    /* main logic, one indent level */
    return process(session, frame);
}
```

Rules:

- No `else` after a `return` / `goto cleanup` / `break` / `continue`.
- Keep cleanup paths consolidated with `goto out;` / `goto err;`
  when several success/failure points need to release the same
  resources — that is the canonical C idiom and is preferred over
  duplicating cleanup before each `return`.
- A function deeper than three indentation levels is a smell — extract a
  helper or invert the conditions.

---

## 6. `const` correctness

Use `const` everywhere it is true. It documents intent, helps the
compiler optimise, and catches accidental mutation at compile time.

- Pointer parameters that the function does not write through must be
  `const T *p`. Read-only buffers, header tables, configuration
  snapshots — all `const`.
- Distinguish `const T *p` (data is read-only, pointer can be
  reassigned) from `T * const p` (pointer is fixed, data is writable)
  from `const T * const p` (both). Pick the strictest form that fits.
- Local variables computed once and not reassigned should be `const`.
  In particular: pointers cached from a struct field at the top of a
  function (`http2_stream_t *const stream = session->current;`).
- String literals are passed as `const char *`. Functions that store or
  forward a literal must accept `const char *` — never cast it away.
- Returning `const T *` from a getter means the caller must not modify;
  do not silently strip `const` at the call site.

The only acceptable way to drop `const` is at a documented FFI boundary
where the third-party API has the wrong signature, with a one-line
comment explaining why.

---

## 7. C standard

The codebase targets **C11** (`-std=c11`) as the baseline, matching
PHP 8.x core. Use C11 features freely:

- `static_assert(...)` for compile-time invariants.
- `_Alignof` / `_Alignas` for explicit alignment.
- Anonymous structs / unions inside other structs.
- `<stdatomic.h>` is allowed, but only in the narrow places that
  legitimately cross threads (e.g. cross-thread counters aggregated above
  the per-thread core — see §1.2). Hot single-threaded paths stay
  plain.
- `<stdbool.h>` is used; do not invent project-local `bool` typedefs.
- `<stdint.h>` fixed-width types (`uint32_t`, `int64_t`, `size_t`,
  `uintptr_t`) for any value whose size matters. Never use bare `int`
  or `long` for protocol fields, sizes, or offsets.

Do **not** use:

- C99-only constructs that C11 deprecated or removed.
- GCC/Clang extensions that have no MSVC equivalent without a fallback,
  unless wrapped in a portability macro (we still build on Windows —
  see §6).
- VLAs (variable-length arrays) — they are optional in C11 and absent
  on MSVC.
- Designated-initializer extensions beyond what plain C11 supports
  (no GNU range-init `[0 ... 7] = x`).

C++-style `//` line comments are fine (C99 and later). Mixed
declarations and statements (declare on first use) are fine and
encouraged — declare a variable as close to its use as possible.

---

## 8. Direct field access inside the TU

If a struct is fully visible in the current translation unit (defined in
the same `.c` file, or in a private `_internal.h` that this TU includes),
read its fields **directly**:

```c
session->conn                    /* yes */
http2_session_get_conn(session)  /* no — adds a function call */
```

Getters exist for the **cross-TU** case, where the struct is forward-
declared in a public header and its layout is not visible to the caller.
Inside the owning TU, plain field access is the canonical form and matches
the rest of the file.

---

## 9. Cross-platform

The project must build and run on **Linux, macOS, and Windows**. POSIX-only
patterns are bugs.

### 9.1 Sockets and signals

- Socket descriptors: use `php_socket_t` (from `main/php_network.h`),
  compare against `SOCK_ERR` / `-1` via helpers — not `>= 0`. On Windows,
  `SOCKET` is unsigned `UINT_PTR`.
- Closing sockets: `closesocket()` on Windows, `close()` on POSIX, behind
  `#ifdef PHP_WIN32`.
- `MSG_NOSIGNAL` does not exist on Windows or macOS:
  ```c
  #ifndef MSG_NOSIGNAL
  # define MSG_NOSIGNAL 0
  #endif
  ```
  Use `SO_NOSIGPIPE` on platforms that have it.
- Any networking / fd / signal code change must be reviewed against the
  Windows build before submission.

### 9.2 Filesystem and paths

POSIX FS APIs are not portable. Do not call them directly when a PHP
or platform-neutral wrapper exists.

- **stat / lstat:** use `php_sys_lstat`, `php_sys_stat`, `VCWD_STAT`,
  `VCWD_LSTAT`, and the `zend_stat_t` / `php_win32_ioutil_stat_t` types.
  Raw `struct stat` plus `lstat()` will not link on MSVC and produces
  wrong field semantics under `php_win32_ioutil`.
- **realpath:** use `tsrm_realpath` / `virtual_realpath`. Plain libc
  `realpath()` is POSIX-only and ignores VCWD.
- **open:** wrap flags. `O_CLOEXEC`, `O_NOFOLLOW`, `O_NONBLOCK` do
  not exist on Windows. Always guard with `#ifdef`. The Win32
  inheritance equivalent is `O_NOINHERIT`; for binary mode add
  `O_BINARY` on `_WIN32`.
- **HTTP-date parsing:** `strptime()` and `timegm()` are POSIX-only
  and absent on MSVC. Use timelib (`timelib_strtotime`) — it parses
  IMF-fixdate, RFC 850, and asctime portably.
- **fnmatch:** `<fnmatch.h>` and `fnmatch()` do not exist on MSVC.
  PHP ships an internal port in `ext/standard/fnmatch.c` but does not
  export it. Either guard the feature with `#ifdef HAVE_FNMATCH` and
  document the win32 behavior, or vendor a minimal port.
- **PATH_MAX:** not portable. On MSVC it is either absent or
  `MAX_PATH = 260`, which is too small for long paths. Use
  `MAXPATHLEN` from `main/php.h`.
- **gmtime / localtime:** `gmtime_r` (POSIX) / `gmtime_s` (Win32);
  always behind `#ifdef _WIN32`. Bare `gmtime()` is unsafe (TLS-shared
  static buffer).
- **timespec fields in `struct stat`:** `st_mtim` (Linux),
  `st_mtimespec` (macOS), `st_mtime` only (Win32 default). Wrap reads
  in a helper that returns `uint64_t` nanoseconds and falls back to
  seconds × 1e9 on Win32. Document the lower entropy on Win32 if it
  affects correctness (e.g. ETag salt).
- **inode (`st_ino`):** meaningless on Win32 unless populated from
  `BY_HANDLE_FILE_INFORMATION`. Do not rely on it as an entropy source
  in cross-platform code without a Win32 fallback.
- **Path separators:** on Windows both `'/'` and `'\\'` are valid
  separators. Segment walks, extension finders, and basename helpers
  must use `IS_SLASH(c)` (which checks both) or be guarded
  `#if !defined(_WIN32)`. Bailing only on `'/'` will silently treat
  `a\b\c` as one segment and bypass per-segment validation. URL
  decoders MUST reject literal `\` and `%5C` to keep this from being
  a smuggling vector regardless of segment-walker portability.
- **POSIX file ownership (`st_uid` / `st_gid`):** not meaningful on
  Win32. Any feature gated on owner equality (anti-symlink-attack
  walks, suid-style checks) must either be `#if !defined(_WIN32)`
  with a documented fallback policy, or implemented through Win32
  ACLs. Silent fail-open or fail-closed is unacceptable.

### 9.3 Compiler intrinsics

- `__builtin_expect`, `__attribute__((...))`, `typeof`, statement
  expressions are GCC/Clang-only. Either go through the Zend macro
  (`EXPECTED`, `UNEXPECTED`, `ZEND_ATTRIBUTE_*`) or wrap in a
  `_MSC_VER` fallback.
- `__thread`: GCC/Clang. MSVC uses `__declspec(thread)`. Use the
  project macro, not the raw spelling.

---

## 10. Error handling and validation boundary

Validate at **system boundaries** only: user input, network bytes,
external API responses, configuration loaded from disk. Inside the core,
trust internal invariants and framework guarantees — do not add defensive
checks for cases that cannot happen.

Do not wrap allocation results in elaborate fallback chains. On hot paths,
an `emalloc` failure goes through Zend's bailout. Cold paths get a direct
`UNEXPECTED(p == NULL)` return.

Do not add backwards-compatibility shims, feature flags, or "transitional"
code paths when all call sites are in-tree and can be changed atomically.

---

## 11. API surface — register-group cohesion

When extending an internal API by adding a new function pointer, group it
into the existing `*_register` family by **return-type family**, not by
surface similarity of the caller-side concept.

A function returning `zend_async_io_t*` belongs in `zend_async_io_register`
next to its return-type siblings (`io_create`, `read`, `write`,
`udp_sendto`, `udp_recvfrom`), even if the concept feels conceptually
separate. Splitting register groups is justified only when the new
function returns an object of a **different family** than what the
existing register covers.

When the change is strictly additive across an API surface that we own
end-to-end (no external consumers), prefer **extending the existing
register signature** (append at the end) over introducing a parallel
register.

---

## 12. Build flags

When configuring with HTTP/3 support, also pass `--enable-http2`. The two
flags must be set together; the generated `config.nice` is not a reliable
record on its own. CI and local builds always include both when H3 is on.

---

## 13a. Formatting and block structure

The mechanical layout is owned by `.clang-format` (LLVM base, 4-space
tabs, 100-column limit, K&R braces on control statements,
`AllowShortIfStatementsOnASingleLine: false`). Run clang-format on
every commit; PR review will reject manual deviations. Beyond what the
formatter enforces:

### 13a.1 Blank lines between logical blocks

Pack-the-lines style is unreadable. Insert one blank line between
logically distinct steps inside a function:

- between local declarations and the first statement that uses them,
- between a guard clause and the work that follows it,
- between a syscall / lookup and its result-check,
- between independent state mutations.

Bad:

```c
if (req == NULL || req->headers == NULL) {
    return NULL;
}
const zval *const zv = zend_hash_str_find(req->headers, name, name_len);
if (zv == NULL) {
    return NULL;
}
```

Good:

```c
if (UNEXPECTED(req == NULL || req->headers == NULL)) {
    return NULL;
}

const zval *const zv = zend_hash_str_find(req->headers, name, name_len);
if (zv == NULL) {
    return NULL;
}
```

Two consecutive blank lines is the cap (`MaxEmptyLinesToKeep: 2`).

### 13a.2 Braces are mandatory

Every `if` / `else` / `for` / `while` / `do` body uses braces, even if
it is a single statement and even if it fits on one line. This rule
predates clang-format in the codebase; the formatter just enforces it.
The reason is the Apple `goto fail` class of bug — a future edit that
adds a line under a brace-less `if` silently changes control flow.

```c
if (state == NULL) return;          /* no */
if (state == NULL) {                /* yes */
    return;
}
```

### 13a.3 Function length

A function over ~80 lines is a smell. Split state machines by phase
(`*_validate`, `*_dispatch`, `*_finalize`) rather than nesting one
600-line `case` switch. Keep the main body at one indentation level
(see §5).

### 13a.4 Comments — additional rules

Beyond §4:

- Never reference current-task PR numbers / fix IDs *inside the
  function body*. Issue references in **module-header banners** (top
  of `.h` / `.c`) are acceptable for traceability of large features
  and stay out of the hot reading path. Per-line "fix for #123"
  comments rot.
- A comment that restates the next line in English is noise. Delete.
- Multi-paragraph comments are justified only for: a published
  invariant the type system cannot express; a non-obvious security
  argument; a measured performance trade-off with a number attached.
  Anything else collapses to one line or is removed.

---

## 13b. Naming for security predicates and partial returns

Extends §3.

### 13b.1 Type / variable parity

A local of type `foo_handler_t` is named `handler` (or `handler_*`),
not `mount`, not `h`, not `x`. If the variable name disagrees with
the type's domain word, one of them is wrong — fix it before merging.
The type rename is usually the right answer: if every call site
spells the variable `mount`, the type wants to be `foo_mount_t`.

### 13b.2 Security predicates state the failure mode

A function whose return value gates access (allow / reject) must read
as a verb-of-decision, not a neutral comparison.

Bad: `path_segments_owner_match()` — the reader cannot tell whether a
return of `false` means "ownership differed (reject)" or "no
ownership data available (don't know)". Both are plausible from the
name.

Good: `verify_path_owner_chain()`, `path_owner_chain_is_trusted()`,
`reject_if_path_owner_foreign()`. The decision the predicate makes is
in the verb.

This rule is mandatory for any function called from a security gate
(symlink policy, open-basedir, auth check, sandbox enter).

### 13b.3 First-of-many returns

A function that returns *one* value from a multi-valued resource —
HTTP headers like `Set-Cookie` / `Via` / `Forwarded`, env keys with
duplicates, hash-table buckets with collisions — must say so:
`get_first_*`, `find_first_*`. A bare `find_request_header` that
silently drops every value past index 0 is a footgun: the next
developer will use it for `Accept-Encoding` and ship a bug.

If both shapes are needed, ship both: `find_first_request_header` and
`find_all_request_headers`.

---

## 13c. Decoders and validation of user-controlled input

Every parser / decoder / validator that consumes bytes from the wire
or from a config file is a security boundary.

### 13c.1 Length and depth caps

Cap the input *before* you start the work, not in the middle of the
loop:

- maximum total length (request URI, header value, body),
- maximum segment / element count (path depth, header count,
  comma-list entries),
- maximum nesting / recursion depth.

`PATH_MAX` is not a cap — it is a buffer size. If your decoder
overflows the buffer and returns 400 BAD_REQUEST, that is an
acceptable failure mode but it does not replace an explicit cap.

### 13c.2 Reject, do not normalize, ambiguous bytes

In a decoder for a security-relevant grammar:

- reject `%00` and literal NUL,
- reject literal `\\` and `%5C` (Windows separator) when path
  semantics are at stake,
- reject overlong UTF-8 / non-shortest forms,
- reject `..` and `.` segments — do not collapse them silently,
- reject zero-length segments (`//`).

"Forgiving" decoders create smuggling between the layer that decodes
and the layer that uses the result.

### 13c.3 Do not reinvent without checking PHP-core, then check
portability of what you find

Before writing a new decoder / formatter, look in PHP first:

- `php_url_decode` exists — but **decodes `%00` and accepts `\\`**.
  Not safe for path use; use a hand-rolled stricter decoder.
- `timelib` (ext/date) parses HTTP-date portably — prefer it over
  `strptime` + `timegm` (which do not exist on MSVC).
- `tsrm_realpath` / `virtual_realpath` over libc `realpath`.
- `MAXPATHLEN` over `PATH_MAX`.
- `php_sys_lstat` / `VCWD_STAT` over raw `lstat` / `stat`.
- ext-based MIME tables: not in core. Hand-rolled is the canonical
  approach (see `sapi/cli/php_cli_server.c` for prior art); align
  the table with cli-server where overlap exists.

Document in the file header which PHP utility was rejected and why
(safety / portability / measured cost), so the next reviewer does
not propose the swap again.

### 13c.4 C unit tests are mandatory for decoders

Every byte-consuming function in a `.c` whose name contains
`decode`, `parse`, `validate`, `match`, `lookup` against
user-controlled input MUST have a cmocka unit test in `tests/unit/`
covering at minimum:

- empty input,
- minimum-length valid input,
- maximum-length valid input,
- one byte past the buffer cap,
- every documented rejection (NUL, `\`, `..`, malformed escape,
  truncated escape, overflow),
- mixed case where case-insensitivity is claimed,
- platform-divergent inputs (separator `\` on Windows; locale-
  affected bytes when `strftime`-class APIs are involved).

A new decoder without these tests does not pass review.

---

## 13d. Filesystem access and TOCTOU

A `lstat`-walk followed by a plain `open(path, ...)` is the textbook
TOCTOU pattern and is forbidden in any code path that is reachable
from the network.

Acceptable forms:

- **Linux 5.6+:** `openat2()` with
  `RESOLVE_NO_SYMLINKS | RESOLVE_BENEATH` against an open root fd —
  the kernel enforces containment atomically.
- **Portable:** `openat(O_NOFOLLOW)` per segment, advancing through
  parent fds opened with `O_DIRECTORY | O_NOFOLLOW`. The root fd is
  obtained once at startup.
- **Last-resort post-open verification:** open the file, then
  `fstat` it, then re-`stat` the path and compare `dev`/`ino`. If
  they diverge — reject. This catches the swap that happened between
  walk and open.

Plain `lstat` plus plain `open(path)` does not qualify, even with
`O_NOFOLLOW` on the open: the intermediate components were already
followed by `open`, and the lstat-walk is a separate syscall sequence
from the open.

A defensive `realpath` after open is good in addition to one of the
above, never instead.

---

## 13e. Defensive checks: where they belong

Extends §10.

A NULL check at the top of an internal helper that is never called
with NULL is dead code: it costs an instruction per call, masks bugs
(use-after-free returns silently instead of crashing in the
debugger), and mis-signals the contract. Three rules:

1. **System-boundary functions** (called from PHP userland, from
   network parsers, from config loaders) — keep the check, emit a
   diagnostic, return an error.
2. **Cross-TU public helpers** that document a NULL contract in the
   header (`free`-style: NULL is a no-op) — keep the check; the
   contract is part of the API.
3. **Internal helpers** (file-static, no external callers, invariants
   already guaranteed by callers) — replace `if (p == NULL) return;`
   with `ZEND_ASSERT(p != NULL);`. Asserts vanish in release and
   crash loudly in debug, which is the correct trade.

The `_free`-shaped function is a special case: if the function
documents "NULL is a no-op", the check stays at the top regardless
of caller invariants — that is the contract. Document it.

---

## 14. OpenSSL on dev hosts

OpenSSL 3.5 with ngtcp2 / nghttp3 is installed system-wide into
`/usr/local/` on the canonical dev host, intentionally overriding the
distribution's OpenSSL. The H3 stack is **not** isolated to `/opt/` and is
**not** `rpath`-pinned — the override is the design, not a side effect.
Reinstalls keep `--prefix=/usr/local` and run `ldconfig`. Verify install
order with `ldconfig -p | grep libssl.so.3` (the `/usr/local/lib/...`
entry must come first).
