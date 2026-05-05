# HTTP body compression

Phase 1 — gzip on responses + inbound request decoding, served identically
across HTTP/1.1, HTTP/2 and HTTP/3. Issue
[#8](https://github.com/true-async/server/issues/8).

## Build

`--enable-http-compression` is on by default. The build prefers
`zlib-ng` (≈2-4× the throughput of stock zlib at the same compression
level) and falls back to system `zlib` if the former is not installed.
Pass `--disable-http-compression` to opt out entirely.

```sh
./configure --enable-http-server --enable-http-compression  # default
./configure --enable-http-server --disable-http-compression  # off
```

## Configuration

All five knobs live on `HttpServerConfig` and freeze at
`HttpServer::__construct` — same discipline as the other config setters.

| Setter | Default | Range |
|---|---|---|
| `setCompressionEnabled(bool)` | `true` | — |
| `setCompressionLevel(int)` | `6` | 1..9 (zlib semantics) |
| `setCompressionMinSize(int)` | `1024` | 0..16 MiB |
| `setCompressionMimeTypes(array)` | text whitelist below | non-empty strings |
| `setRequestMaxDecompressedSize(int)` | `10485760` (10 MiB) | ≥ 0 (0 = no cap) |

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

When compression engages, the response gets:

```
Content-Encoding: gzip
Vary: Accept-Encoding         (appended if Vary already exists)
```

`Content-Length` is recomputed for buffered responses; on streaming
responses (`HttpResponse::send`) it is dropped — chunked H1 and H2
DATA framing carry length implicitly.

## Inbound (request body) decoding

`Content-Encoding: gzip` (and the legacy `x-gzip` alias) on incoming
requests is decoded transparently before the handler runs. Handlers
see `HttpRequest::getBody()` returning the decoded payload; the
`Content-Encoding` header on the request side is left intact for
diagnostic round-trip.

| Outcome | HTTP status |
|---|---|
| Unknown coding (e.g. `br`, `deflate`) | 415 Unsupported Media Type |
| Decoded size exceeds `request_max_decompressed_size` | 413 Payload Too Large |
| Corrupt inflate stream | 400 Bad Request |
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

Phase 2 will add Brotli (`br`) and zstd (`zstd`) backends through the
same `http_encoder_t` vtable; phase 3 covers pre-compressed static
assets (`*.gz` / `*.br` on disk, served via sendfile). Threadpool
offload for very large buffered bodies is gated on real-world latency
profiles — not added speculatively.

Strict `deflate` is intentionally skipped: half the deployed clients
send raw deflate and the other half send zlib-wrapped deflate, and
neither side reliably negotiates which is which. Use gzip.
