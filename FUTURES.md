# Server features needed for HttpArena production-tier compliance

This file lists C-level features the `true-async-server` extension needs to
add so that the HttpArena entry can:

- pass profiles we currently can't validate (`baseline-h2c`, `json-h2c`),
- and (separately) move from `"type": "tuned"` to `"type": "production"`
  in `frameworks/true-async-server/meta.json`.

Each feature lists *why* HttpArena needs it, *what's missing*, and *what
the API should look like in PHP*. Items are independent — they can be
landed in any order.

---

## 1. Per-listener protocol mask (h2c-only listener)

### Why

HttpArena `baseline-h2c` / `json-h2c` profiles run on port `8082` and the
validator **explicitly checks that the port refuses HTTP/1.1**:

> Validation explicitly checks that port 8082 refuses plain HTTP/1.1
> requests. A server that dual-serves h1 and h2c on the same port would
> let the benchmark measure whichever protocol the client picked —
> useless for ranking.
> *(`docs/test-profiles/h2/baseline-h2c/implementation.md`)*

We already speak h2c — `src/core/http_protocol_strategy.c` recognises the
`PRI * HTTP/2.0` preface and routes into the HTTP/2 strategy. But we
cannot mark a listener "h2 only".

### What's missing

`protocol_mask` is server-wide, not per-listener:

- `src/http_server_class.c:714` — `.protocol_mask = HTTP_PROTO_MASK_ALL`
  on the server object.
- `src/http_server_class.c:930` — `addHttpHandler()` flips on both
  `MASK_HTTP1` and `MASK_HTTP2` for the whole server.
- `src/core/http_protocol_strategy.c:54-55` —
  `detect_and_assign_protocol()` reads the mask from `conn->server`,
  not the listener.

`stubs/HttpServerConfig.php:418` exposes `enableHttp2(bool)` (marked
TODO), but it's also server-wide, not per-listener.

### What needs to change

1. Add `protocol_mask` to the listener descriptor next to `host` /
   `port` / `tls`.
2. Expose via PHP. Two reasonable shapes:
   - extend signature: `addListener($host, $port, $tls, $protocols = ALL)`,
   - or add a dedicated method:
     ```php
     public function addH2cListener(string $host, int $port): static {}
     ```
3. In `detect_and_assign_protocol()` read the mask from
   `conn->listener` rather than `conn->server`. Keep the server-wide
   mask as the union of all listener masks (still useful as a hot-path
   short-circuit).
4. When a listener is h2-only and the first bytes are not `PRI `:
   close the connection, or route into the existing nghttp2
   `BAD_CLIENT_MAGIC` path so the client receives a clean GOAWAY.

### When done

Re-add `"baseline-h2c"` and `"json-h2c"` to
`frameworks/true-async-server/meta.json`, and bind a second listener on
`:8082` in `entry.php` with the h2-only flag.

---

## 2. Built-in static file handler

### Why

HttpArena `production` rules require:

> Static files must be read from disk. No in-memory caching, no
> memory-mapped files, no pre-loaded file buffers. […] Compression must
> use the framework's standard middleware or built-in static file
> handler — no handmade compression code. Serving pre-compressed
> `.br`/`.gz` variants from disk **is allowed**, but only through a
> documented framework API […]. No custom file-suffix lookup logic.

Our current `entry.php` slurps `/data/static` into a hash table and
hand-rolls a `.br`/`.gz` chooser — that's allowed for `tuned`, but for
`production` we have nothing to point at as "the framework's documented
API" because no such API exists.

### What's missing

`HttpServer` / `HttpServerConfig` have no static-file primitive — every
URL is dispatched to a single PHP callback registered via
`addHttpHandler()`.

### What needs to change

Add a built-in static handler at the C level, configured before the
server starts. Suggested API:

```php
$config->serveStatic(
    string $urlPrefix,        // e.g. "/static/"
    string $rootDirectory,    // e.g. "/data/static"
    bool $precompressed = false,   // pick up sibling .gz/.br on disk
    bool $followSymlinks = false,
): static;
```

Behaviour expected from the C implementation:

- Per-request `open()` of the file. No PHP-side caching (so we satisfy
  production rules verbatim). `sendfile(2)` / `splice(2)` zero-copy
  where the platform supports it.
- Built-in MIME table covering at least the HttpArena set: `css`, `js`,
  `html`, `woff2`, `svg`, `webp`, `json`, `txt`, `wasm`, `ico`, `xml`.
  This replaces the user-land `pathinfo()` lookup — the spec calls
  user-land suffix logic a violation.
- When `precompressed` is true and the request's `Accept-Encoding`
  includes `br` or `gzip`, look for `<file>.br` / `<file>.gz` next to
  the original and serve that with `Content-Encoding`.
- `404` for unknown files. `416`/`206` for `Range:`. Strong `ETag` and
  `If-None-Match` (cheap-to-compute: inode + mtime + size).
- Path traversal protection (reject `..`, NUL, absolute paths).
- Routes registered through `serveStatic` short-circuit before user
  handlers — no PHP frame on the hot path.

### When done

Replace the entire `if (str_starts_with($path, '/static/'))` block in
`entry.php` with one `$config->serveStatic('/static/', '/data/static',
precompressed: true)` call. The hand-written prelude that scans
`/data/static` and reads files into memory disappears.

---

## 3. Transparent response compression middleware — *shipped*

Phase 1 (issue #8) landed the response-pipeline compression filter with
gzip: registry, `Accept-Encoding` negotiation, request decoder, response
encoder, and `setCompressionLevel()` on `HttpServerConfig`. See
`src/compression/` and `docs/COMPRESSION.md`.

Phase 2 (issue #9) extends the same `http_encoder_t` vtable with Brotli
and zstd as drop-in codecs — no architectural changes. Tracked on
GitHub; not duplicated here.

`entry.php` no longer needs hand-rolled compression for `/json/`, and
the `json-comp` profile can be listed in `meta.json` once #9 lands.

---

## 4. (Optional) Built-in routing primitive

### Why

Not required by HttpArena production rules — `addHttpHandler()` with a
single dispatch callback is allowed everywhere. Listed here only so
it's not forgotten.

### What's missing

No equivalent of `route('GET', '/baseline11', $callback)` at the
config level. Today everything goes through one callback with manual
`if`-chains.

### Suggested API

```php
$config->route(string $method, string $path, callable $handler): static;
$config->route(['GET', 'POST'], '/baseline11', $handler);
```

Internally: a small radix tree or hash on `(method, path)` checked
before the catch-all `addHttpHandler` callback, so static endpoints
don't pay PHP-call cost when they could be served from C.

### When this would help

- `/pipeline` and `/baseline11` are the hottest paths in the bench
  suite. If the route can be answered (or a fixed body returned)
  without ever entering PHP, throughput on those two profiles jumps.
- Cleaner user code in `entry.php` — but that's secondary.

---

## Summary table

| # | Feature | Unlocks profile(s) | Required for production tier? |
|---|---|---|---|
| 1 | Per-listener protocol mask + `addH2cListener` | `baseline-h2c`, `json-h2c` | No (independent) |
| 2 | Built-in static handler | `static`, `static-h2`, `static-h3` | **Yes** |
| 3 | Built-in routing | None directly — perf only | No |

Compression middleware (formerly item 3) shipped via issues #8/#9 — see
section 3 above. Without (2) we cannot legitimately classify as
`production` for the static profiles; until it lands, `meta.json` stays
at `"type": "tuned"`.
