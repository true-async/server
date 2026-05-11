# Built-in static file handler — design + implementation plan

Tracks FUTURES.md item #2. Unblocks HttpArena `production` tier on the
static profiles (`static`, `static-h2`, `static-h3`). Required because
production rules forbid user-land MIME lookup, in-memory caching, and
hand-rolled compression — none of which `entry.php` can replace today
because the framework has no static API.

This document is the load-bearing context for resuming the work after a
clean session. Read it end-to-end before touching code.

---

## 1. Surface (PHP API)

Builder class + register-on-server method. Mirrors the existing
`HttpServerConfig` + `HttpServer` split: config-as-object, behavior on
the server.

```php
namespace TrueAsync;

final class StaticHandler {
    public function __construct(string $urlPrefix, string $rootDirectory) {}

    // index / fallthrough
    public function setIndexFiles(string ...$files): static {}      // default ['index.html']
    public function disableIndex(): static {}
    public function setOnMissing(StaticOnMissing $mode): static {}  // NotFound | Next

    // precompressed sidecars
    public function enablePrecompressed(string ...$encodings): static {}  // 'br', 'gzip', 'zstd'
    public function disablePrecompressed(): static {}

    // security
    public function setDotfilePolicy(StaticDotfiles $p): static {}     // Deny (default) | Allow | Ignore
    public function setSymlinkPolicy(StaticSymlinks $p): static {}     // Reject (default) | Follow | OwnerMatch
    public function hide(string ...$globs): static {}

    // cache / headers
    public function setEtagEnabled(bool $enabled): static {}           // default true, weak
    public function setCacheControl(string $value): static {}
    public function setHeader(string $name, string $value): static {}  // declarative, no callbacks

    // dir listing
    public function setBrowseEnabled(bool $enabled): static {}         // default false

    // MIME overrides
    public function setMimeType(string $extension, string $contentType): static {}

    // getters for introspection (omitted here — generated boilerplate)
}

enum StaticOnMissing { case NotFound; case Next; }
enum StaticDotfiles  { case Deny;     case Allow; case Ignore; }
enum StaticSymlinks  { case Reject;   case Follow; case OwnerMatch; }

// Registered on server, parallel to addHttpHandler
$server->addStaticHandler(StaticHandler $h): static;
```

Multiple `addStaticHandler` calls are allowed — each pinned to its own
URL prefix. Order of registration = order of prefix-match attempts.

Why a builder class and not `array $options`: typo'd string keys
silently misconfigure (Express footgun); IDE autocomplete + per-setter
validation (`enablePrecompressed('foo')` throws immediately, not at
`start()`); presets reusable as factories. Symmetric to existing
`HttpServerConfig`.

`setHeader()` is declarative (header-map evaluated in C). **No PHP
callable per-request** — that re-enters the VM and kills the no-coroutine
fast path.

---

## 2. Where it dispatches

**Insertion point: `src/core/http_connection.c::http_connection_dispatch_request`,
after `request_zv`/`response_zv` are built and stream ops attached, but
**before** `ZEND_ASYNC_NEW_COROUTINE` (line ~1614).**

Current flow today:

```
http_connection_dispatch_request(conn, req):
   1. ecalloc(ctx)                                      :1573
   2. http_request_create_from_parsed → request_zv      :1577
   3. object_init_ex(response_zv)                       :1582
   4. install h1_stream_ops                             :1591
   5. http_compression_attach                           :1601
   6. set_default_json_flags                            :1609
   7. ZEND_ASYNC_NEW_COROUTINE  ← spawn coroutine       :1614
   8. coroutine->internal_entry = http_handler_coroutine_entry
   9. http_server_on_request_dispatch                   :1638
  10. ZEND_ASYNC_ENQUEUE_COROUTINE                      :1649
```

New flow inserts step 6.5:

```c
if (UNEXPECTED(server->static_handler_count > 0)) {
    const http_static_result_t rc =
        http_static_try_serve(server, conn, ctx, req);

    if (rc == HTTP_STATIC_HANDLED) {
        /* C-handler owns ctx; lifecycle ends in uv_fs callback chain. */
        return;                       /* NO coroutine spawned */
    }
    if (rc == HTTP_STATIC_ERROR) {
        /* 4xx already written; emit short response without coroutine. */
        http_static_emit_short_error(conn, ctx);
        return;
    }
    /* PASSTHROUGH → fall through to coroutine + PHP handler. */
}
/* steps 7-10 unchanged */
```

**Why this point**: `http_request_t` is fully parsed, response object
exists, stream ops are bound. We are in a libuv read-callback context
(`ZEND_ASYNC_CURRENT_COROUTINE == NULL` — see `http_connection.c:1502`)
where suspend isn't allowed but registering further async ops via
`uv_fs_*` / `ZEND_ASYNC_*` callbacks is fine. The C handler is pure FSM:
no coroutine alloc, no `zend_try` setup, no PHP-VM entry, no context
switch.

Symmetric insertion for HTTP/2 (before `ZEND_ASYNC_NEW_COROUTINE` at
`src/http2/http2_strategy.c:156`) and HTTP/3 (before
`ZEND_ASYNC_NEW_COROUTINE` at `src/http3/http3_dispatch.c:134`). PR #1
ships H1 only; H2/H3 land in PR #2.

**Lifecycle hook**: cleanup steps currently done in
`http_handler_coroutine_dispose` (response flush, keep-alive re-arm,
`http_server_on_request_dispose`, OBJ_RELEASE on `ctx` zvals) must run
on the static path too. Extract a shared helper
`http_request_finalize(conn, ctx)` invoked from both the coroutine
dispose and the static FSM tail (after `uv_fs_close`).

---

## 3. C-level architecture

**`src/static/`** — new package.

| File | Purpose |
|---|---|
| `static_handler_class.c` | PHP class: ctor, setters with validation, getters, lock-on-attach |
| `http_static_dispatch.c`  | `http_static_try_serve()` — prefix match, FSM driver |
| `http_static_path.c`      | URL-decode → traversal guard → `realpath()` → root-prefix check |
| `http_static_mime.c`      | Built-in MIME table (HttpArena set: css/js/html/woff2/svg/webp/json/txt/wasm/ico/xml) + per-mount overrides |
| `http_static_etag.c`      | Weak ETag from `(mtime_ns, size, ino)`; `If-None-Match` / `If-Modified-Since` |
| `http_static_range.c`     | Range parser, single + `multipart/byteranges`, `If-Range` (PR #3) |
| `http_static_precompressed.c` | `.br/.gz/.zst` sibling lookup, reuses `compression/http_compression_negotiate.c` (PR #4) |
| `http_static_serve.c`     | uv_fs_open → fstat → read-loop → write-into-response-stream → close |

**Storage on `http_server_object`**:

```c
typedef struct {
    zend_string *url_prefix;           /* "/static/" — interned where possible */
    size_t       url_prefix_len;       /* memcmp fast path */
    zend_string *root_directory;       /* canonicalized at attach time */
    zend_string *cache_control;        /* nullable */
    HashTable    extra_headers;        /* set via setHeader(), evaluated in C */
    HashTable    mime_overrides;       /* ext → content-type */
    zend_string **hide_globs;          /* compiled patterns */
    size_t        hide_count;
    zend_string **index_files;
    size_t        index_count;
    uint32_t      flags;               /* DOTFILES_*, SYMLINKS_*, PRECOMP_BR/GZ/ZSTD, ETAG, BROWSE, ON_MISSING_NEXT */
} http_static_handler_t;

/* in http_server_object: */
http_static_handler_t *static_handlers;   /* ecalloc'd array */
size_t                 static_handler_count;
```

Bit-flags over a struct of bools — one cache-line load on the hot path,
not eight.

**FSM, no coroutine**:

```
try_serve():
  prefix-match (single linear scan; usual count <= 3)
  ↓
  build absolute path: root || (req->path - prefix)
  validate: no "..", no NUL, no "%00", dotfile policy, hide globs
  realpath() (or O_NOFOLLOW + fstat for symlink reject mode)
  ↓
  uv_fs_open(O_RDONLY | O_CLOEXEC, callback)
  ↓ on_open:
    if ENOENT → 404 / passthrough per on_missing
    if EACCES → 403
  uv_fs_fstat(fd, callback)
  ↓ on_fstat:
    weak ETag = mtime_ns ^ size ^ ino  (single 64-bit mix)
    If-None-Match / If-Modified-Since  → 304 + early return
    Range header                       → range FSM (PR #3)
    format status + headers (single iovec build, single send)
  ↓
  read-write pump:
    uv_fs_read(fd, buf=64K, off, callback)
    on_read: stream_ops->push_chunk(response, buf, n) [H1: uv_write callback]
    advance offset; loop until size reached
  ↓
  uv_fs_close(fd, callback)
  ↓ on_close: http_request_finalize(conn, ctx) — same path as PHP-handler dispose
```

For HTTP/2 and HTTP/3, the file is plumbed as an nghttp2/nghttp3 **data
provider** rather than a chunk push: the library calls our provider when
its flow window allows; we kick `uv_fs_read` and return DEFERRED; on
read completion we resume the stream. Natural fit, zero coroutine.

---

## 4. Optimality requirements

The static path is **hot** — bench profiles hammer `/static/main.css`
millions of times. Every microsecond in our handler is a microsecond
that doesn't go to the next request.

### Syscall minimization
- **One `open(O_RDONLY|O_CLOEXEC|O_NOFOLLOW?)`** per file. No probe-stat
  before open. No reopen for compressed sibling without first checking
  Accept-Encoding (skip the syscall when client doesn't accept br/gz).
- **One `fstat`** per request — gives mtime, size, ino, mode, all from
  the open fd. Don't `stat(path)` *and* `fstat(fd)`.
- **Headers + body coalescing**: H1 plain — try `writev(2)` (uv_write
  with two iovecs: headers buf + first read chunk) so the TCP stack
  packetizes them together. Avoid Nagle/cork dance for single-write
  small files.
- **No per-request directory walk**. Index file resolution: stat each
  candidate once and cache *negative* results within a single request
  (don't re-stat after 404).
- **`SO_LINGER` / keep-alive re-arm** — no extra syscall on success
  path; reuse what `http_request_finalize` already does for PHP path.
- **`sendfile(2)` / `splice(2)`** — PR #5 only, when not TLS-userspace
  and not encoding-on-the-fly. MVP uses `uv_fs_read` + write; sendfile
  is incremental.

### Memory minimization
- **Read buffer 64 KiB**, allocated **per-request** (not per-chunk),
  freed in close-callback. One `emalloc` for the buffer, one for the
  headers iovec.
- **Headers buffer** built once into a `smart_str` at fstat time, sent
  once; never reallocated mid-request.
- **No PHP zval allocation** on the static path past what
  `dispatch_request` already built (`request_zv`, `response_zv`). No
  `zend_string` creation per request — all per-mount strings (prefix,
  root, cache_control) are owned by the handler struct, refcounted.
- **No HashTable lookups in the hot loop**. MIME map is consulted once
  (at fstat-time after extension extraction); extension lookup is a
  fixed-size perfect-hash or sorted-array binary search over the built-in
  table, falling back to per-mount override HashTable only on miss.
- **Stack-allocated path buffer**: `char path[PATH_MAX]` on stack for
  resolve, copied to allocated `zend_string` only if passed onward
  (logging). Avoid PATH_MAX-on-stack only on platforms where it's huge
  (Hurd ≥ 8 KiB) — cap at 4096 then heap-fallback.

### CPU minimization
- **Branch hints on the hot path**: `EXPECTED(server->static_handler_count == 0)` at the
  early skip; `UNEXPECTED(rc == HTTP_STATIC_ERROR)` for error paths;
  `EXPECTED(file_found)` for the success branch. The compiler already
  does PGO-shaped guesses, but explicit hints win on the dispatcher
  prefix-match loop.
- **`const` everywhere meaningful**: `const http_static_handler_t *`
  parameters in helpers; `const char *path`; `const zend_string *prefix`.
  Lets the compiler keep values in registers across calls and proves
  intent to readers.
- **No allocation in the prefix-match loop**: `memcmp` on
  `(req->path, sh->url_prefix, sh->url_prefix_len)`. No `zend_string`
  comparison helpers (those branch on interned vs not, refcount, etc.).
- **Single ETag mix**: `etag = mtime_ns ^ ((uint64_t)size << 17) ^ ino`,
  formatted as 16 hex chars. No SHA, no MD5 — they cost too much per
  request and add no security value here.
- **Validate options at attach time** (`addStaticHandler`), not per
  request: precompressed encoding list precomputed into a bitmask;
  cache_control header pre-formatted into an immutable buffer; index
  file list materialized once.
- **Skip body decompression on static path**: `http_compression_attach`
  is currently called before our hook (line :1601). For static the call
  is wasted (GET/HEAD have no body to decode). PR #1 leaves it; PR #2
  considers reordering: do `http_static_try_serve` *before*
  `http_compression_attach` — saves a function call + branch on every
  static request.
- **No PHP call_user_func anywhere** on the static path. No
  `zend_call_function`, no `zend_is_callable`, no fcc cache lookup.
- **Bailout firewall**: not needed on the static path — pure C, no
  PHP-VM. `zend_try` is omitted entirely.

### Concurrency / contention
- **Per-mount handler array is read-only after `start()`**: locked via
  the same mechanism as `HttpServerConfig`. Workers see their own copy
  via the existing `transfer_obj` machinery — no shared mutable state,
  no atomics in hot path.
- **No global FD cache** in MVP. Each request opens its own fd.
  `open_file_cache`-style optimization is deferred (PR #6+) and must be
  per-worker (no cross-worker contention).
- **No userland syscall serialization**. `uv_fs_*` runs on the libuv
  thread pool; the read callback fires on the loop thread without
  locks.

### Coding-standard checks
- C: see `docs/CODING_STANDARDS.md`. `const` on pointers and arguments
  by default. `UNEXPECTED`/`EXPECTED` on the hot dispatcher and FSM
  branches. `static` on every file-local function.
- No `printf`-family in hot path — use SAPI logger only on errors.
- `pemalloc` only for cross-request lifetime data (per-mount config);
  `emalloc` for per-request (path buffer, read buffer, iovec).
- No `ZEND_ASSERT` on hot path beyond cheap pointer-non-null checks
  (asserts are no-ops in release builds, but readability still costs).

---

## 5. PR breakdown

| PR | Scope | Blocks |
|---|---|---|
| **#1** | `StaticHandler` class + stub + arginfo. `addStaticHandler` on `HttpServer`. Storage in `http_server_object`. Dispatch hook in `http_connection_dispatch_request` (H1 only). MIME table. Path traversal guard. Weak ETag, conditional GET (304). `uv_fs_open/fstat/read/close` chain → write to response stream. `http_request_finalize` extracted. PHPT: 200, 304, 404, 403, traversal blocked, MIME, dotfile-deny default. | — |
| **#2** | H2/H3 integration: nghttp2/nghttp3 data-provider hookup. PHPT: same matrix on H2 and H3. | #1 |
| **#3** | Range support: single byte-range, suffix range, `multipart/byteranges`, `If-Range`. 416 emission. PHPT: range matrix incl. invalid syntax. | #1 |
| **#4** | Precompressed sidecars `.br` / `.gz` / `.zst`. Reuse `compression/http_compression_negotiate.c` for `Accept-Encoding` parsing. Range over encoded bytes. | #1, #3 |
| **#5** | `sendfile(2)` / `splice(2)` zero-copy fast path. Plain HTTP/1, no encoding-on-the-fly only. Behind a config flag for first release. | #1 |
| **#6** | `browse` (dir listing) HTML index. Optional, lowest priority. | #1 |

**After PR #1+#2+#3+#4 land**: rewrite `entry.php` to drop the in-memory
static map and the hand-rolled `.br`/`.gz` chooser. Mark
`frameworks/true-async-server/meta.json` as `"type": "production"` for
the static profiles. This closes FUTURES.md item #2.

---

## 5a. Status — что сделано, что осталось (2026-05-08)

### Закрыто

PR #1 + PR #5 фактически слились в одну ветку (sendfile попал
вместе с базовой инфраструктурой через расширение zend_async API):

- ✅ `StaticHandler` class + 3 enum'а + полный набор setter'ов с
  валидацией (`enablePrecompressed('foo')` throws at setter, locks
  on attach, header injection blocked, etc.).
- ✅ `addStaticHandler` на `HttpServer`, storage на server-object,
  static-only deployments (без addHttpHandler) работают.
- ✅ Dispatch hook в `http_connection_dispatch_request` с тремя
  результатами (PASSTHROUGH / HANDLED soft-skip / HARD_ZERO).
- ✅ MIME table (44 расширения, sorted-array binary search,
  precomputed lengths, debug-build sorted-assert).
- ✅ Path resolution (percent-decode, traversal guard, dotfile
  policy, hide-globs via fnmatch, **backslash reject** для
  Windows-семантики, **realpath prefix-check** против symlink
  traversal через intermediate components).
- ✅ Weak ETag + conditional GET (RFC 9110 §13.1.2 wildcard, weak-
  equal compare, IMF-fixdate / RFC 850 / asctime parsing).
- ✅ HEAD method handling (Content-Length без body).
- ✅ `http_request_finalize` extracted из dispose tail в shared
  helper. Используется coroutine path и hard-zero path.
- ✅ **Hard-zero coroutine path** на plain TCP: callback chain
  `fs_open → io_stat → write headers fire-and-forget → sendfile
  → finalize`. Никакой корутины не спавнится.
- ✅ zend_async API v0.11 (отдельный PR в php-src + ext/async):
  `ZEND_ASYNC_FS_OPEN` (returns pending io_t),
  `ZEND_ASYNC_IO_SENDFILE` (zero-copy uv_fs_sendfile с partial
  loop). Вместе с CHANGELOG'ом.
- ✅ Soft-skip path (для TLS, on_missing: Next, directory + index
  walk) сохранён как always-correct fallback.
- ✅ **Headers + sendfile ordering** — TCP_CORK gate в `ss_kick_off`
  с unconditional uncork в `ss_finalize` (fix 67eadb9). Linux-only
  через `#ifdef TCP_CORK`; на других платформах — no-op (race
  теоретический там, поскольку без TCP_CORK кернел сериализует
  uv_write/sendfile через одну очередь сокета).
- ✅ **`If-Modified-Since`** PHPT — 005-static-if-modified-since.phpt
  покрывает IMS strictly-after / before / equal-to mtime
  (RFC 9110 §13.1.3) (commit 2c46937).
- ✅ **Telemetry counter** `static_zero_coroutine_total`
  (`http_server_counters_t`) бампается на каждом успешном
  ss_kick_off, exposed через `getTelemetry()`, PHPT
  006-static-zero-coroutine-counter.phpt (commit 2c46937).
- ✅ **Valgrind sweep** — все 6 static PHPTs (001-006) проходят
  под `TEST_PHP_ARGS=-m`, 0 leaked tests (commit 2c46937).
- ✅ PHPT: 001-static-basic (200/304/404/HEAD/conditional/POST
  passthrough), 002-dotfile-and-onmissing, 003-static-security
  (symlink-escape, backslash, NUL, header injection, EACCES
  disclosure), 004-static-on-missing-next, 005-static-if-modified-
  since, 006-static-zero-coroutine-counter, 007-static-tls-streaming.
- ✅ **TLS hard-zero (user-space) — chunked SSL_write FSM**
  (этот коммит). `ss_kick_off` теперь принимает TLS-соединения
  тоже: после headers (atomic_send через
  `tls_fsm_send_plaintext_atomic`) FSM крутит loop
  `ZEND_ASYNC_IO_READ(file_io, 16 KiB)` →
  `tls_fsm_send_plaintext_atomic` (encrypt+kick) → ждать
  `tls_zc_write_done_cb` (новый optional observer на zc-write
  completion) → следующий read. Никакой корутины не
  спавнится — всё в callback context.
  Bench (16 cores WSL2, debug-build, 64 KiB файл, wrk -c64 -t4):
  - **plain TCP sendfile**: 19327 req/s, 1.18 GB/s, 3.1 ms median
  - **TLS chunked SSL_write**: 4843 req/s, 303 MB/s, 12.7 ms median
  Соотношение 25% — ожидаемый user-space AES baseline на debug-
  build; release-build даст 1.5-2× благодаря AES-NI inlining.
- ✅ **Open file cache** (commits 81bc752, 0aab165, 8f05e8d, e328522
  + perf 35e4963). Per-handler конфиг `StaticHandler::setOpenFileCache(
  maxEntries, ttlSeconds=60)` (off by default). Реализация: HashTable
  + LRU + TTL, hit/miss telemetry counters, hand-formatted IMF date /
  ETag (snprintf убран из hot path). На cache hit FSM пропускает
  IO_STAT, etag-format, MIME lookup, IMF-date format — open остаётся
  ради async fd для sendfile. Bench (debug-zts, 64 KiB файл,
  wrk -c64 -t4 -d5s, warm dentry, 3-run avg):
  - cache **off** : ~16287 req/s
  - cache **on**  : ~19622 req/s (~99.98% hits) — **+20%**
  Закрывает Open Question 1 («defer until bench shows it matters»)
  на opt-in базе.
- ✅ **Symlink::OwnerMatch** (commit 7302ea7). Per-segment lstat/stat
  sweep — symlink на каждом сегменте path должен иметь owner == owner
  target'а, иначе 404. За компанию закрыта дыра REJECT в hard-zero
  на final-segment симлинках внутри mount (ZEND_ASYNC_FS_OPEN не
  отдает O_NOFOLLOW; помог явный lstat в pre-flight). PHPT 009.
- ✅ **Precompressed sidecars `.br/.gz/.zst`** (commit eb18e1f, PR #4).
  При совпадении Accept-Encoding с enabled-codings и существующим
  sibling-файлом — fs_path swap'ится на sidecar, FSM отдаёт
  сжатые байты, headers содержат `Content-Encoding` + `Vary`.
  Server preference zstd > br > gzip — reuse существующего
  `http_compression_negotiate`. Кэш ключ включает суффикс. MIME
  считается по оригинальному пути и проносится через override.
  PHPT 010.
- ✅ **Range support (single, plain TCP)** (commit 6c99358, PR #3
  partial). RFC 9110 §14.2: `bytes=A-B`, `bytes=A-`, `bytes=-N`.
  If-Range strong-equal compare. 206 / 416 / 200-fallthrough.
  Headers: `Accept-Ranges: bytes`, `Content-Range`. Sendfile с
  offset+len. Multi-range и multipart/byteranges → fall through к
  200 (§14.2 разрешает). TLS Range fallthrough'ит на 200
  (`ZEND_ASYNC_IO_READ` без offset — отдельная async-API extension).
  PHPT 011.

### Осталось

| Acceptance / Plan item | Status | Notes |
|---|---|---|
| Bench `wrk -c 256 -t 4 -d 30 /static/...` vs `entry.php` | partial | bench tooling готов, цифры выше для file-static; entry.php сравнение out-of-codebase |
| TLS streaming abrupt-close race | known issue | wrk-style массовый abrupt close с in-flight TLS streaming → assertion в `conn_arena_cleanup` (alive list не пустой при teardown). PHPT 007 чистый, штатный keep-alive close — чистый. Только при `wrk --timeout 0` с десятками одновременных RST. Требует pass над shutdown chain — отдельный fix. |
| **PR #2** H2/H3 интеграция | not started | nghttp2/nghttp3 data-provider hookup |
| **PR #3 follow-up** multi-range / multipart-byteranges | not started | low priority — большинство клиентов делают single-range |
| **PR #3 follow-up** TLS Range | blocked on async-API | нужен `ZEND_ASYNC_IO_READ` с offset (или pread-equivalent) |
| **PR #6** Browse listing | not started | lowest priority |
| Rewrite `entry.php` + flip `meta.json` to production | blocked | требует #2 + #3 + #4 |

### kTLS fast-path — заблокирован архитектурой BIO

TODO 5a description предполагает быстрый TLS-вариант: «если kTLS TX
engaged, повторно использовать sendfile-путь, потому что ядро шифрует
in-place». В текущей кодовой базе это **dead code**:

- `tls_session_new` (`src/core/tls_layer.c:640`) привязывает SSL через
  `BIO_pair` (memory BIOs), а не socket BIO. Comment в
  `src/core/tls_layer.c:944` явно фиксирует, что в этой layout-е
  `BIO_get_ktls_send` всегда возвращает 0 — ядро не может перехватить
  записи, потому что OpenSSL вообще не пишет на сокет напрямую
  (ciphertext попадает в network_bio, оттуда — fire-and-forget
  uv_write).
- Поэтому `tls_session_ktls_tx_active` всегда false → ветка
  «engaged → sendfile» не срабатывает на этом сервере.
- Включение настоящего kTLS требует переподключения SSL на socket BIO
  (либо `SSL_set_fd`), что меняет всю модель read FSM (он живёт за
  счёт того, что мы кормим OpenSSL ciphertext'ом из BIO pair). Это
  отдельный архитектурный PR с последствиями для read-FSM и
  destroy-gating.

Counter `tls_ktls_tx_total` существует как hook на будущее, но в
текущей конфигурации никогда не бампается.

### Что переехало в зависимый PR

PR #5 (sendfile) технически реализован, но **через расширение
zend_async API в php-src** (не через config-flag в этом репо). API
v0.11 живёт в:

- `~/php-src` ветка `true-async` (commits `618e8d222f`, `67aa465162`,
  `aab16cb5ba`, `c5a8f083d3`)
- `~/php-src/ext/async` ветка `main` (commits `3312c56`, `73571f4`,
  `0167a5c`, `0ff08d5`, `99e7695`)

ABI breaking — поднята до 0.11.0. Описание в
`ext/async/CHANGELOG.md` под секцией [0.7.0].

### splice(2) — отложен

splice требует промежуточного pipe'а (fd→pipe→fd семантика, не
file→socket как sendfile). Без реального consumer'а с конкретными
требованиями (request-body→storage / fd-to-fd proxy) — оставляем
sendfile единственным zero-copy примитивом до появления реального
use case.

---

## 6. Acceptance checklist

- [x] `StaticHandler` class compiles, stubs regenerate cleanly, arginfo
      hash matches.
- [x] `addStaticHandler` rejects calls after `start()` (через
      `server->running` — функционально эквивалентно `isLocked`).
- [x] `enablePrecompressed('foo')` throws `InvalidArgumentException` at
      setter time, not at start time.
- [x] Path traversal: `/static/../etc/passwd`, `/static/%2e%2e/x`,
      `/static/foo%00.html`, absolute paths, NUL — all 400 or 404.
- [x] Symlink reject mode: 404 on symlinks; follow mode: serves;
      owner-match: per-segment lstat/stat sweep, реализован в
      commit 7302ea7. PHPT 009 покрывает same-owner accept;
      cross-owner mismatch требует root и не testable в PHPT.
- [x] `If-None-Match` matches → 304 with empty body, weak ETag echoed.
- [x] `If-Modified-Since` past mtime → 304 — реализован, PHPT
      покрывает (005-static-if-modified-since.phpt, commit 2c46937).
- [x] Dotfile policy default `Deny` → 404 on `/.git/config`.
- [x] `on_missing: Next` falls through to `addHttpHandler` callback;
      default `NotFound` returns 404 in C without PHP entry.
- [x] Telemetry counter `static_zero_coroutine_total` confirms zero
      coroutines spawned for matched static requests
      (006-static-zero-coroutine-counter.phpt, commit 2c46937).
- [ ] Bench: `wrk -c 256 -t 4 -d 30 /static/main.css` baseline vs
      current `entry.php` map. (out-of-codebase)
- [x] No new memory leaks under valgrind on the static PHPT suite
      (`TEST_PHP_ARGS=-m`, commit 2c46937). 1M-request soak still
      pending — needs dedicated harness.

---

## 7. Open questions parked for later

1. ~~`open_file_cache`-style per-worker FD/stat cache~~ ✅ закрыто
   (commits 81bc752 / 0aab165 / 8f05e8d / e328522). Per-handler
   opt-in; на cache hit FSM пропускает IO_STAT/etag/MIME/IMF-date.
   Bench подтвердил +20% на warm dentry.
2. Per-listener static handler scoping (admin port wants no static).
   Currently global, like `addHttpHandler`. If needed, add `listeners:
   [...]` option later — not breaking.
3. `browse` (dir listing) — defer to PR #6, low priority.
4. `setHeader` interaction with conditional GET — закрыт: extra
   headers fire on 304, кроме `Content-*` family (RFC 9110 §15.4.5).
5. ~~**TLS hard-zero (user-space)**~~ ✅ закрыто (этот PR).
   Реализовано через chunked SSL_write FSM с новым phase'ом
   `SS_PHASE_TLS_READ` / `SS_PHASE_TLS_DRAIN` и observer hook
   `conn->tls_zc_write_done_cb` для pacing на write-completion.
6. **TLS abrupt-close shutdown race**: при wrk-style массовых RST
   с in-flight streaming, `conn_arena_cleanup` срабатывает
   assertion `alive_head == NULL && "live conns at cleanup"`.
   Static FSM в SS_PHASE_TLS_DRAIN не финализуется потому что
   loop остановлен до ответа от `tls_zc_write_done_cb`. Нужен
   cancellation pass на server stop: для каждого live conn в
   static FSM — force ss_finalize. Не блокер для PHPT/штатного
   serving.
7. **kTLS architectural rewire**: для активации kTLS нужно
   переключить SSL с BIO_pair на socket BIO. Отдельный design
   open — read FSM и destroy-gating придётся переписать. См.
   §5a "kTLS fast-path — заблокирован архитектурой BIO".
8. **`SYMLINKS_OWNER`** реальная реализация — fstat после open,
   сравнение st_uid файла с st_uid lstat'а каждого segment'а.
   TOCTOU acceptable (требует write-access на хосте).
9. **Open file cache** (HashTable + LRU): nginx-style
   open_file_cache. Сейчас каждый запрос — sync stat + realpath
   на hot path. Кеш `путь → (st, mtime, content_type)` с LRU
   bound и mtime invalidation на hit сэкономит ~3-4 µs на
   запрос на warm cache. Полезно при hot serving одних и тех же
   файлов; на холодном set — нейтрально. Отдельный PR.

---

## 8. Anchor file references (for resume)

- Insertion site: `src/core/http_connection.c:1562` (`http_connection_dispatch_request`).
- Coroutine spawn to skip: `src/core/http_connection.c:1614` (`ZEND_ASYNC_NEW_COROUTINE`).
- Existing handler invocation (for parallel structure): `src/core/http_connection.c:1786` (`zend_call_function`).
- Compression hook order reference: `src/core/http_connection.c:1601` (`http_compression_attach`).
- Dispose to fold into `http_request_finalize`: `http_handler_coroutine_dispose` (search same file).
- Listener mask reference: `src/http_server_class.c:1190-1213`.
- Handler registration template: `src/http_server_class.c:944-966` (`addHttpHandler` — pattern for `addStaticHandler`).
- Config-class template: `src/http_server_config.c` (whole file is the model for `static_handler_class.c`).
- Stub generation: `stubs/HttpServerConfig.php` + `_arginfo.h` — copy pattern for `stubs/StaticHandler.php`.
- Coding standards: `docs/CODING_STANDARDS.md`.
