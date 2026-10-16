# Request rate limit: design

A configurable request rate limit inside the server: a request over its limit is answered `429 Too Many Requests`
with `Retry-After` by C, before PHP. Asked for by the fiction library on IFCastle (EdmondDantes/lib, plan S26.20):
a limiter in PHP pays routing and a PHP call for every refused request, and a database-backed counter costs 44 ms a
request there (measured 2026-10-16). The owner wants the key configurable: the client address, a session cookie or
an auth token.

Revision 2, after Critic round 1 (table at the end).

## PHP API

Rules are value objects added to the server before `start()`; `addRateLimit` validates and locks the rule (later
setters on it throw, as `http_static_handler_lock`), and the server freezes its rules with the rest of the config.

```php
$server->addRateLimit((new RateLimit(requests: 600, perSeconds: 60))       // per client address, all paths
    ->name('ip'));
$server->addRateLimit((new RateLimit(requests: 120, perSeconds: 60))
    ->name('session')
    ->paths('/api/works', '/api/search')         // decoded path prefixes, segment-bounded
    ->methods('GET', 'HEAD')
    ->keyByCookie('lib_session'));               // or keyByHeader('authorization'); default keyByPeer()
$config->setTrustedProxies(['10.0.0.0/8', '::1']); // X-Forwarded-For from these peers names the client
```

- `RateLimit(int $requests, int $perSeconds)`: token bucket, `requests` per `perSeconds`; `burst(int)` sets the
  bucket size, default `requests`. Validated: requests and burst 1..10^6, perSeconds 1..86400, and the bucket time
  `perSeconds·burst/requests` at most 2^62 ns (B4).
- `paths(string ...$prefixes)`: up to 32 prefixes starting with `/`; a prefix matches the decoded path when equal
  to it or followed by `/`. `methods(string ...)`: tokens. Default: every path and method.
- `excludeStatic(bool = true)`: requests served by a static handler are not counted. Default true.
- `maxKeys(int)`: 1024..2^20, default 65536. The sum over all rules of a server is at most 2^21 slots (48 MiB).
- `name(string)`: shown in telemetry; default `rule<N>`.

## Keys

- `keyByPeer()` (default): the client address. An IPv4-mapped or -compatible IPv6 address is un-mapped to its 4
  bytes first (B3); IPv6 is keyed by its /64, `ipv6Prefix(48..128)` changes it. The address family and length are
  part of the hashed key. A peer on a Unix socket has no address: rules keyed by peer do not apply to it (a local
  socket carries a local proxy, which names the client with X-Forwarded-For when trusted).
- With `setTrustedProxies(list of CIDR)` (at most 64 entries): when the peer is inside one of them, the client
  address is the rightmost address of `X-Forwarded-For` that is not itself trusted; a malformed header, or none,
  keeps the peer. Without the list the header is ignored.
- `keyByCookie(string $name)`: the first cookie of that name in the `cookie` header, split on `;` with spaces
  trimmed, as the app reads it. A value that contains `%`, `"`, or is empty or longer than 512 bytes is not used: the
  request falls back to the peer key of the same rule. The app URL-decodes cookie values, so an encoded value would
  give one session many keys (B1).
- `keyByHeader(string $name)`: the header value as stored; a value that holds a comma (the server joins repeated
  headers with `, `, http_request.c:1298-1325), is empty or longer than 512 bytes falls back to the peer key (I9).
- **A cookie or header key shapes clients that cooperate.** A client can send a new invented value on every request
  and get a new bucket each time. Such a rule is therefore always paired with a peer-keyed rule over the same paths;
  `start()` refuses a server whose cookie or header rule has no peer-keyed rule covering its paths and methods (B1).

## Evaluation

All rules whose path and method match are checked first; tokens are taken from every one of them only if all pass
(I1). A refusal names the rule with the longest wait, and `Retry-After` is that wait. The path is the
percent-decoded path of the request target up to `?` (an absolute-form target loses its scheme and authority);
a malformed `%` sequence is refused with 400 before the limiter, as the router would refuse it (B2).

## Algorithm and memory

Token bucket per key, the arithmetic of `ws_publish_allowed` (ws_session.c:877-915): credit in nanoseconds,
`cost = perSeconds·1e9 / requests`, `cap = cost·burst` (validated below 2^62), `credit = min(cap, credit + now -
stamp)` computed without overflow, clock `http_now_coarse_ns()`. `Retry-After = ceil((cost - credit) / 1e9)`, at
least 1.

Each rule owns one table per server instance, allocated with `pemalloc` when the server starts (each worker thread
has its own server instance): `maxKeys` slots of `{uint64 tag; uint64 credit_ns; uint64 stamp_ns}`. A key hashes
with SipHash-2-4 under a random key per server to 64 bits, forced odd so 0 marks a free slot; 4-way probing from
`hash mod maxKeys`. When all 4 slots hold other keys, the one whose bucket is full again takes the new key, else the
one with the oldest stamp; that key starts again from a full bucket, so a takeover only ever gives the evicted key
more tokens. `requests_rate_limit_evictions_total` counts takeovers.

No locks: one table per server instance (P2.1). With `setWorkers(N)` each worker has its own tables, so a key gets
up to N times the limit when its requests land on different workers; H3 in reactor mode may spread one connection's
requests over workers under load (http3_dispatch.c:164-191). USAGE.md states both.

## Where it runs

- H1 (http_connection.c:2864), H2 (http2_strategy.c:212), H3 local (http3_dispatch.c:627) and the worker side of
  H3 reactor mode (worker_dispatch.c:1247): the check runs before the request and response objects and the
  coroutine are created, and a refusal finalises the stream without a coroutine, the way the static HARD_ZERO path
  does (http_connection.c:2923, http3_dispatch.c:614-623). If that path cannot carry a 429 on some protocol, the
  check moves into `http_request_prepare_dispatch` for that protocol and the cost is measured and written down (I2).
- H1 keeps the connection; a refused request whose body is still unread past 64 KiB gets `Connection: close`.
- gRPC (`content-type: application/grpc*`): HTTP 200 with trailers-only `grpc-status: 8` (RESOURCE_EXHAUSTED) and
  `grpc-message`, on every transport (I4).
- WebSocket upgrade (http_connection.c:179) and H2 CONNECT (http2_strategy.c:240): their own check after the 426/400
  validation and their own 429 emitter with the right reason phrase and Retry-After; the H1 upgrade reject closes the
  connection as every upgrade reject does (I3).
- Static files: counted only by rules with `excludeStatic(false)`; H3 reactor mode serves static on the reactor
  thread, where no rule is checked, so such rules do not cover reactor-served static files (I6).

## Observability

Counters in the X-macro table (php_http_server.h:1057, its static assert) and `getTelemetry()`
(http_server_class.c:5615), reset with the others (:5777): `requests_rate_limited_total`,
`requests_rate_limit_evictions_total`. A refused request counts in `total_requests` and `responses_4xx_total`
like any 429.

## Out of scope

- PROXY protocol; `Forwarded` (RFC 7239).
- Limits per account resolved in PHP; exact limits across worker threads.

## Tests

- cmocka `tests/unit/core/test_rate_limit.c`: bucket and Retry-After at the validated bounds (overflow), refill,
  burst, probing and takeover, free-slot tag, address un-mapping and /64, X-Forwarded-For rightmost untrusted,
  cookie parsing (first wins, `%` and `"` refused, H2 cookie crumbs), header with a comma, percent-decoded path and
  segment-bounded prefixes, check-then-commit over two rules, SipHash test vectors.
- phpt `tests/phpt/server/core/0xx-rate-limit-*.phpt`: validation and the start-time pairing check; H1 and TLS H1
  over the limit: 429 + Retry-After, handler not run, connection kept, a pipelined request after a refused POST;
  recovery; cookie and header keys with fallback; `/api/%77orks` limited; IPv4 on a `[::]` listener keyed per
  address; Unix listener; trusted proxy; H2; H3 local and reactor (static excluded); gRPC status 8; WebSocket 429
  reason and Retry-After; telemetry counters.

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
