# HTTP body compression

Phase 1 — gzip on responses + inbound request decoding, served identically
across HTTP/1.1, HTTP/2 and HTTP/3. Issue
[#8](https://github.com/true-async/server/issues/8).

Phase 2 ([#9](https://github.com/true-async/server/issues/9)) plugs Brotli
(`br`) and zstd (`zstd`) into the same `http_encoder_t` vtable. No
architectural changes — the response pipeline asks the negotiation
layer for a codec, the registry returns the matching vtable, and
encoding proceeds. Codecs missing from the build are simply absent from
the registry; negotiation degrades to the next preference.

## Build

`--enable-http-compression` is on by default and provides the gzip
backend. The build prefers `zlib-ng` (≈2-4× the throughput of stock
zlib at the same compression level) and falls back to system `zlib` if
the former is not installed. Pass `--disable-http-compression` to opt
out entirely.

`--enable-brotli` and `--enable-zstd` are also on by default and
auto-detect their respective libraries via pkg-config (`libbrotlienc +
libbrotlidec`, `libzstd`). When the library is missing the build emits
a warning and continues without the codec — gzip alone is enough for
basic compliance with HttpArena's `json-comp` profile, but brotli/zstd
typically deliver 15-25% better ratios on JSON.

```sh
./configure --enable-http-server                            # all three (auto-detect)
./configure --enable-http-server --disable-brotli           # gzip + zstd only
./configure --enable-http-server --disable-zstd             # gzip + brotli only
./configure --enable-http-server --disable-http-compression # off (also disables br/zstd)
```

`HttpServerConfig::getSupportedEncodings()` reports which codecs the
running build was actually compiled with — useful for production-config
audits and for skipping codec-specific tests cleanly.

## Configuration

All five knobs live on `HttpServerConfig` and freeze at
`HttpServer::__construct` — same discipline as the other config setters.

| Setter | Default | Range |
|---|---|---|
| `setCompressionEnabled(bool)` | `true` | — |
| `setCompressionLevel(int)` | `6` | 1..9 (zlib / gzip) |
| `setBrotliLevel(int)` | `4` | 0..11 |
| `setZstdLevel(int)` | `3` | 1..22 |
| `setCompressionMinSize(int)` | `1024` | 0..16 MiB |
| `setCompressionMimeTypes(array)` | text whitelist below | non-empty strings |
| `setRequestMaxDecompressedSize(int)` | `10485760` (10 MiB) | ≥ 0 (0 = no cap) |

### Example

```php
use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;

$config = (new HttpServerConfig())
    ->addListener('0.0.0.0', 8080)
    ->setCompressionEnabled(true)         // master switch
    ->setCompressionLevel(6)              // gzip
    ->setBrotliLevel(4)                   // brotli
    ->setZstdLevel(3)                     // zstd
    ->setCompressionMinSize(1024)
    ->setRequestMaxDecompressedSize(10 * 1024 * 1024);

// Health-check what the build actually shipped with:
foreach (HttpServerConfig::getSupportedEncodings() as $enc) {
    echo "supported: $enc\n";
}

$server = new HttpServer($config);
$server->addHttpHandler(function ($req, $resp) {
    $resp->setHeader('Content-Type', 'application/json')
         ->setBody(json_encode(['ok' => true, 'echo' => $req->getBody()]))
         ->end();
});
$server->start();
```

A request with `Accept-Encoding: gzip, br, zstd` lands brotli or zstd
ahead of gzip without any handler-level changes; one with
`Accept-Encoding: gzip` still gets gzip. Bodies smaller than
`compression_min_size` ship as identity regardless of negotiation.

### Per-codec level setters

`setCompressionLevel` keeps its gzip-only meaning. A unified setter was
considered and rejected — the level scales differ enough (gzip 1..9,
brotli 0..11, zstd 1..22) that a linear mapping would lose the high end
of brotli/zstd. Defaults are picked for production-typical usage:
brotli quality 4 is ~5–10× faster than 11 with marginal ratio loss;
zstd 3 is the zstd team's own production default (better ratio than
gzip-6 at higher throughput).

Default MIME whitelist (replaces wholesale on `setCompressionMimeTypes`):

```
application/javascript    image/svg+xml      text/javascript
application/json          text/css           text/plain
application/xml           text/html          text/xml
```

`getCompressionMimeTypes()` returns the live, materialised list — what
`var_dump($cfg->getCompressionMimeTypes())` shows is exactly the policy
the negotiation code applies.

## Per-response opt-out

```php
$response->setNoCompression();
```

Overrides every other rule (Accept-Encoding negotiation, MIME match,
size threshold). Use on:

- responses that combine secrets with reflected user input (BREACH
  mitigation),
- pre-compressed payloads where the handler already set
  `Content-Encoding`,
- diagnostic dumps you want to read off the wire as-is.

## Negotiation

Follows RFC 9110 §12.5.3 with two pragmatic deviations:

1. **No `Accept-Encoding` header → identity only.** RFC permits any
   coding in this case, but real-world clients without AE are usually
   probes / scripts that may not handle gzip. Matches nginx.
2. **`identity;q=0` and `*;q=0` are honoured.** A `*;q=0` without a
   later identity entry excludes identity, so the response goes out as
   identity if there is no acceptable coding — the 406 path is not
   taken; preference is to ship a working response.

Skip rules — when **any** of these holds, the response stays identity:

- request method is `HEAD`
- request carries a `Range` header
- response status ∈ `1xx, 204, 304`
- handler already set `Content-Encoding`
- response `Content-Type` is outside the whitelist
- response body is smaller than `compression_min_size` (buffered path
  only — streaming bodies have unknown size)
- `setNoCompression()` was called on the response
- `compression_enabled` is false in the config

Server-side preference order is `zstd > br > gzip > identity`, applied
to whatever the client lists in `Accept-Encoding`. q-value-based client
preference is not honoured exactly — the server-side order covers ~99%
of real-world traffic, and decoupling prevents pathologies like a
client preferring `br` over `gzip` by 0.001 on a build where brotli is
absent. Codecs missing from the build skip silently to the next
preference; clients that listed only the missing codecs degrade to
identity (or 406 if `identity;q=0` was set, but the implementation
prefers a working response over a strict 406).

When compression engages, the response gets:

```
Content-Encoding: <gzip | br | zstd>
Vary: Accept-Encoding              (appended if Vary already exists)
```

`Content-Length` is recomputed for buffered responses; on streaming
responses (`HttpResponse::send`) it is dropped — chunked H1 and H2
DATA framing carry length implicitly.

## Inbound (request body) decoding

`Content-Encoding: gzip`, `br`, `zstd` (and the legacy `x-gzip` alias)
on incoming requests are decoded transparently before the handler
runs. Handlers see `HttpRequest::getBody()` returning the decoded
payload; the `Content-Encoding` header on the request side is left
intact for diagnostic round-trip. Each decoder enforces the same
`request_max_decompressed_size` cap with the same growth schedule
(4 KiB initial, doubling under cap), so an attacker cannot pick a
codec to bypass the bomb cap.

| Outcome | HTTP status |
|---|---|
| Unknown coding (e.g. `deflate`, custom) | 415 Unsupported Media Type |
| Decoded size exceeds `request_max_decompressed_size` | 413 Payload Too Large |
| Corrupt inflate / brotli / zstd stream | 400 Bad Request |
| `identity` or no `Content-Encoding` header | pass-through |

## Streaming

When handlers stream via `$response->send($chunk)`, the encoder is
installed transparently on the first call (subject to negotiation).
The wrapper accumulates compressed output across an entire encoder
iteration and ships it as a single underlying chunk — one chunked-H1
size line, one H2 DATA frame per `send()` call, regardless of how many
internal inflate passes deflate needed.

`mark_ended()` (called by `$response->end()`) drains the gzip trailer
(CRC32 + ISIZE) into a final chunk before delegating to the underlying
ops.

## Engine selection

The build banner reports the chosen engine:

```
checking for zlib-ng... yes (version 2.1.0)
```

or

```
checking for zlib-ng... no
checking for zlib (fallback)... yes (version 1.3)
```

At runtime the engine is also visible via the
`http_compression_engine_name()` C symbol — `"zlib-ng"`, `"zlib"`, or
`"disabled"` when the feature is off.

## What's not in scope (yet)

Phase 3 covers pre-compressed static assets (`*.gz` / `*.br` / `*.zst`
on disk, served via sendfile). Threadpool offload for very large
buffered bodies is gated on real-world latency profiles — not added
speculatively. q-value-based client preference is also tracked but
not yet wired in; server-side preference covers ~99% of real
Accept-Encoding strings.

Strict `deflate` is intentionally skipped: half the deployed clients
send raw deflate and the other half send zlib-wrapped deflate, and
neither side reliably negotiates which is which. Use gzip.
