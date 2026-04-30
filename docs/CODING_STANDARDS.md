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

## 13. OpenSSL on dev hosts

OpenSSL 3.5 with ngtcp2 / nghttp3 is installed system-wide into
`/usr/local/` on the canonical dev host, intentionally overriding the
distribution's OpenSSL. The H3 stack is **not** isolated to `/opt/` and is
**not** `rpath`-pinned — the override is the design, not a side effect.
Reinstalls keep `--prefix=/usr/local` and run `ldconfig`. Verify install
order with `ldconfig -p | grep libssl.so.3` (the `/usr/local/lib/...`
entry must come first).
