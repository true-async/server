# Request rate limit: design

A configurable request rate limit inside the server: a request over its limit is answered `429 Too Many Requests`
with `Retry-After`, and the PHP handler does not run. Asked for by the fiction library on IFCastle
(EdmondDantes/lib, Fog after S26.20): the owner wants it in the server, configurable, and keyable by the client
address, a session cookie or an auth token.

Revision 3, after Critic rounds 1 and 2 (tables at the end). Two PRs:

- **PR 1**: rules keyed by the client address, trusted proxies, refusal through the skip-handler path at all four
  dispatch sites, telemetry, tests.
- **PR 2**: cookie and header keys, with the pairing check (a client-chosen key always under a peer rule).

## PR 1

### PHP API

Rules are value objects added to the server; `addRateLimit` validates the rule and locks it (later setters throw, as
`http_static_handler_lock`), and the rules travel to worker clones with the static mounts (http_server_class.c:6683,
6817-6821). Trusted proxies are config, set before `new HttpServer($config)` locks it (http_server_class.c:1860) and
frozen with the shared config (http_server_config.c:3316, populate_from_shared :3555, release :3484).

```php
$config->setTrustedProxies(['127.0.0.1', '::1', '10.0.0.0/8', 'unix']);
$server = new HttpServer($config);
$server->addRateLimit((new RateLimit(requests: 600, perSeconds: 60))->name('ip'));
$server->addRateLimit((new RateLimit(requests: 120, perSeconds: 60))->name('reads')
    ->paths('/api/works', '/api/search')->methods('GET', 'HEAD'));
```

- `RateLimit(int $requests, int $perSeconds)`: token bucket; `burst(int)` sets the bucket size, default `requests`.
  Validated: requests and burst 1..10^6, perSeconds 1..86400, bucket time `perSeconds·burst/requests` at most
  2^62 ns.
- `paths(string ...)`: up to 32 prefixes; each starts with `/` and has no trailing `/` except `/` itself, which means
  every path. P matches a path that equals P or starts with P followed by `/`. `methods(string ...)`: tokens; HEAD is
  its own method. Default: every path and method.
- `maxKeys(int)`: 1024..2^20, default 65536; the sum over a server's rules is at most 2^21 slots (48 MiB per worker).
- `name(string)`: for telemetry; default `rule<N>`.
- `setTrustedProxies(list)`: up to 64 entries, each an IPv4 or IPv6 CIDR or the word `unix`. The trusted address
  changes only the limiter's key: `getRemoteAddress()` and the access log keep the peer.

### The client address

1. The peer: an IPv4-mapped IPv6 address (`::ffff:0:0/96`) is un-mapped to its 4 bytes; nothing else is rewritten
   (`::1` stays `::1`). Trusted CIDR entries given in mapped form are normalised the same way.
2. A peer inside the trusted list (or a Unix-socket peer with `unix` in the list): `X-Forwarded-For` (repeated
   headers arrive joined with `, `, http_request.c:1310) is walked from the right. Each element is trimmed of spaces
   and tabs and parsed as `a.b.c.d`, `a.b.c.d:port`, `[v6]`, `[v6]:port` or a bare v6 address, then un-mapped as in
   step 1. A trusted element is skipped; the first element that is not trusted is the client. When the walk reaches
   an element that does not parse (`unknown`, `_obf…`, empty), or the header is absent or holds only trusted
   elements, the client is the last trusted hop. Elements left of the stop are never parsed.
3. A Unix-socket peer without `unix` in the trusted list, or with it but no usable header, is keyed as the single
   address `unix`, so a peer rule still limits it (one shared bucket), and `start()` logs that once.
4. IPv6 clients are keyed by their /64; `ipv6Prefix(48..128)` changes it. Family and length are part of the hashed
   key.

### Matching and evaluation

The path is the request target up to `?` (an absolute-form target loses scheme and authority), percent-decoded for
matching only. A target with a malformed `%` sequence matches every rule that has paths, so it is limited by all of
them; its response is not changed. All matching rules are checked first; tokens are taken from every one only if all
pass. A refusal reports the longest wait among the refusing rules as `Retry-After`.

### Algorithm and memory

Token bucket per key with the arithmetic of `ws_publish_allowed` (ws_session.c:877-915): credit in nanoseconds,
`cost = perSeconds·1e9 / requests`, `cap = cost·burst`, `credit = min(cap, credit + (now - stamp))` with the
difference clamped to `cap` before the add, clock `http_now_coarse_ns()`. `Retry-After = ceil((cost - credit) /
1e9)`, at least 1.

One table per rule per server instance (each worker clone has its own), `pecalloc`'d at start: `maxKeys` slots of
`{uint64 tag; uint64 credit_ns; uint64 stamp_ns}`. Keys hash with SipHash-2-4 under a random key per server to 64
bits forced odd (0 marks a free slot); 4-way probing from `hash mod maxKeys`. When the 4 slots hold other keys, the
one whose bucket is full again is taken, else the oldest stamp; a takeover only gives the evicted key more tokens.
No locks (P2.1). With `setWorkers(N)` a key can get up to N times the limit; H3 reactor mode may spread one
connection over workers under load (http3_dispatch.c:164-191). USAGE.md states both.

### Where it runs

The check runs where each site decides whether PHP runs, after the request and response objects exist: H1
(http_connection.c:2897-2943), H2 (http2_strategy.c:292-340), H3 local (http3_dispatch.c:713-751), worker side of
H3 reactor mode (worker_dispatch.c:1320-1339). A refusal sets the 429 status, `Retry-After` and a short body on the
response object and sets the site's existing skip flag (`ctx->skip_php_handler`, `stream->skip_handler`,
`s->skip_handler`, `ctx->skip_handler`). The dispose path then sends it with the existing framing, keep-alive,
linger, telemetry and access log; the refused request costs its objects and one coroutine that runs no PHP. A
path without the coroutine is added per protocol only if a measurement asks for it, H2 first.

- Static files: the check runs after `http_static_try_serve`, so a request static served is not counted, and one
  it passed through (`on_missing: Next`, non-GET) is. H3 reactor mode serves static on the reactor thread before any
  rule; USAGE.md says so.
- gRPC: when `grpc_classify` (grpc.c:57-68) says gRPC or grpc-web, the refusal is trailers-only `grpc-status: 8`
  (RESOURCE_EXHAUSTED) with `grpc-message`, through the gRPC response helpers.
- WebSocket upgrade (http_connection.c:179) and H2 CONNECT (http2_strategy.c:240) bypass the sites above: they get
  the same check after their 426/400 validation and answer 429 with the right reason and `Retry-After`; the H1
  upgrade reject closes the connection as every upgrade reject does (ws_dispatch.c:84-107; the 429 reason phrase is
  added to build_error_response, :222-231).

### Observability

`requests_rate_limited_total` and `requests_rate_limit_evictions_total` in the X-macro table (php_http_server.h:1057
and its static assert), `getTelemetry()` (http_server_class.c:5615), the reset (:5777) and the contract test
(telemetry/009). A 429 counts in `total_requests` and `responses_4xx_total`.

### Tests (PR 1)

- cmocka `tests/unit/core/test_rate_limit.c`: bucket and Retry-After at the validated bounds, refill, burst,
  probing and takeover, odd tags; un-mapping (`::ffff:a.b.c.d` yes, `::1` no); X-Forwarded-For walk (ports, `[v6]`,
  `unknown`, `_obf`, empty elements, tabs, all trusted, garbage left of the stop); prefix matching (`/`, `/api`,
  `/api/worksheet`, decoded `%77`, malformed `%`); check-then-commit over two rules; SipHash test vectors.
- phpt `tests/phpt/server/core/0xx-rate-limit-*.phpt`: validation; H1 and TLS H1 429 + Retry-After with the handler
  not run and the connection kept, a pipelined request after a refused POST; recovery after the window; IPv4 on a
  `[::]` listener keyed per address; a trusted proxy with X-Forwarded-For; a Unix listener with and without `unix`;
  path and method filters; static excluded and `on_missing: Next` counted; H2; H3 local and reactor; gRPC status 8;
  WebSocket 429 reason and Retry-After; telemetry counters.

## PR 2: cookie and header keys

`keyByCookie(string $name)` and `keyByHeader(string $name)` on a rule. A client can invent a new value per request,
so a cookie or header rule must be covered by peer rules: for every prefix p of the rule (or "every path") and every
method m (or "every method"), some peer-keyed rule R has R's paths empty or a prefix q with p equal to q or under q
by segments, and R's methods empty or containing m. `addRateLimit` refuses an uncovered rule. The cookie value is the
first cookie of that name, split on `;` and trimmed as the app does; a missing, empty or over-512-byte value, or a
header value with a comma (Digest, repeated headers), falls back to the peer key. Its own Critic pass before code.

## Critic round 1 (2026-10-16)

| # | Remark | Answer |
|---|---|---|
| B1 | an invented cookie or header value gets a fresh bucket; encoded values multiply one session | cookie and header rules must be paired with a peer rule (start() refuses otherwise); `%` and `"` values fall back to the peer |
| B2 | prefixes matched on the raw target; `/api/%77orks` skips the limit; `/api/works` matches `/api/worksheet` | decoded path, malformed `%` refused, segment-bounded prefixes |
| B3 | IPv4 on a dual-stack listener arrives as `::ffff:a.b.c.d` and /64 groups all of IPv4 | un-map first, family in the key; Unix peers exempt from peer rules |
| B4 | `cost·burst` overflows uint64 inside the ranges | perSeconds ≤ 86400 and bucket time ≤ 2^62 ns validated; overflow-free refill |
| I1 | multi-rule debit order | check every rule, then commit all |
| I2 | a refused request still pays a coroutine | refused before objects and coroutine where the HARD_ZERO path allows; measured otherwise |
| I3 | WS and CONNECT bypasses have no response object; 429 reason "Bad Request" | own check and emitter, reason fixed |
| I4 | gRPC needs status 8, call sites order differently | trailers-only grpc-status 8 on every transport |
| I5 | freeze happens at `new HttpServer`, not start | rules are added to the server and locked at add, as static handlers |
| I6 | reactor static bypass, spill across workers | excludeStatic by default; both documented |
| I7 | behind a proxy every client is the proxy | `setTrustedProxies` with X-Forwarded-For rightmost untrusted |
| I8 | no total memory bound, allocator unnamed | pemalloc, 2^20 per rule, 2^21 slots per server |
| I9 | repeated headers are joined, "first value" false | a value with a comma falls back to the peer |
| A | static assets, unread POST body, /48 holders, free-slot sentinel, telemetry, tests | excludeStatic default, Connection: close past 64 KiB, documented, odd tags, counters named, tests added |

## Critic round 2 (2026-10-16)

| # | Remark | Answer |
|---|---|---|
| 1 | HARD_ZERO needs the objects and is deferred; no object-free refusal exists on H1, H3 or the worker | refusal through the existing skip-handler path at all four sites; a no-coroutine path only on measurement |
| 2 | un-mapping IPv4-compatible addresses turns `::1` into 0.0.0.1 | only `::ffff:0:0/96` is un-mapped, before the trust match, for peers, XFF elements and CIDR entries |
| 3 | an exempt Unix peer leaves peer rules empty and B1 open | `unix` can be trusted; otherwise a Unix peer is the single key `unix` |
| 4 | XFF grammar and "malformed" undefined | walk from the right, ports and brackets parsed, the last trusted hop on an unparsable element |
| 5 | "static not counted" needs the static result | the check runs after `http_static_try_serve` |
| 6 | the router does not refuse a malformed `%`; refusing it changes responses | such a target matches every path-scoped rule; responses unchanged |
| 7 | prefixes `/` and `/api/` | no trailing `/` except `/` itself, which means every path |
| 8 | the pairing check is ambiguous | defined per prefix and method over the union of peer rules; moved to PR 2 |
| 9 | trusted proxies on a locked config; effect on getRemoteAddress | set before `new HttpServer`, frozen with the shared config; limiter key only |
| 10 | "unread past 64 KiB" undefined at headers-complete | gone: the skip-handler path drains or lingers as any response does |
| A | grpc_classify, cookie trim, Digest commas, per-worker memory, pecalloc | taken |

## Critic round 3 on PR 1 (2026-10-16)

The four sites, their flags, pipelining after a small refused POST, header joining, the arithmetic, config freezing
and the rule copy to workers were checked and hold. All findings accepted; PR 1 reads with these answers.

| # | Remark | Answer |
|---|---|---|
| 1 | on H1 the check at dispatch runs after the whole body is buffered or written to disk | for methods static never serves (not GET or HEAD) the verdict is taken in on_headers_complete next to the admission shed (http_parser.c:418-423) and stored on the request; a refused request skips the multipart open and the body buffer, on_body counts and drops the bytes, and the dispatch site sends the stored 429 without a second check. GET and HEAD keep the check after `http_static_try_serve` |
| 2 | H2 extended CONNECT has no validation step and no response object | the check runs in `http2_strategy_dispatch` before the WebSocket branch (:240); a refused CONNECT takes the normal path and gets a 429 HEADERS with END_STREAM (a non-2xx rejects extended CONNECT, RFC 8441) |
| 3 | gRPC status and H1 | gRPC on H2, H3 and the worker: `:status 200`, trailers-only with `grpc-status: 8`, `grpc-message` and `grpc-retry-pushback-ms`, set through `http_response_ensure_grpc_status`; such a refusal counts as 2xx. H1 (grpc-web included) always gets a plain 429 |
| 4 | body streaming: the request is released while llhttp still feeds its body (suspected use-after-free) | answered by 1: a refused streaming upload never reaches dispatch; a phpt covers a chunked POST refused mid-body and a pipelined GET after it, and any existing skip path that shows the fault is fixed in this PR |
| 5 | `//` and dot segments reach a route without matching a prefix | a rule matches when the decoded path or its normalised form (`//` collapsed, `.` and `..` resolved) matches |
| A | malformed `%` vs methods, 404 order, Unix detection by family, ipv6Prefix placement, method case, addRateLimit while running, tables only on serving instances, WebSocket telemetry, the start() log | only the path condition counts as met; the check runs before the no-handler 404 and only when static did not handle the request; AF_UNIX by `ss_family`, `peer_len == 0` its own never-trusted key; `ipv6Prefix` is a rule setter; methods are upper-cased; `addRateLimit` while running throws; tables are allocated by the instance that serves; the H1 WebSocket reject calls `http_request_telemetry`; start() logs only "Unix listener without `unix` trusted" |
| T | test gaps | added: XFF from an untrusted peer ignored; setWorkers(2) enforcing rules; setTrustedProxies after `new HttpServer` and addRateLimit while running throw; H2 CONNECT refused; gRPC on each transport and grpc-web over H1; refused multipart POST with no body buffered and keep-alive kept; body streaming refused mid-body; two rules where one refuses, through the server |
