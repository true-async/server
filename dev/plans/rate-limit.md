# Request rate limit: design

A configurable request rate limit inside the server: a request over its limit is answered `429 Too Many Requests`
with `Retry-After` by C, before the PHP handler, the static handler or a WebSocket upgrade runs. Asked for by the
fiction library on IFCastle (EdmondDantes/lib, plan S26.20): a limiter in PHP still pays a coroutine, routing and a
PHP call for every refused request, and a database-backed counter costs 44 ms a request there (measured
2026-10-16).

## PHP API

A rule is a value object, frozen at `start()` like `StaticHandler` (http_server_class.c:1937, :6407):

```php
$config->addRateLimit(
    (new RateLimit(requests: 120, perSeconds: 60))   // token bucket: 2 per second, 120 at once
        ->burst(30)                                  // optional bucket size; default = requests
        ->paths('/api/works', '/api/search')         // path prefixes; default: every path
        ->methods('GET', 'HEAD')                     // default: every method
        ->keyByCookie('lib_session')                 // or keyByHeader('authorization'); default keyByPeerIp()
        ->maxKeys(65536)                             // table size of this rule; default 65536
);
```

- `keyByPeerIp()`: the raw 4- or 16-byte address of `req->peer`, no port (as the H3 peer map,
  http3_listener.c:175-182). An IPv6 peer is keyed by its /64, so one host cannot rotate addresses inside its
  prefix; `ipv6Prefix(int $bits)` changes it (48..128).
- `keyByCookie(string $name)`: the value of that cookie in the `cookie` header; `keyByHeader(string $name)`: the
  first value of that header (http_request_find_header, http_request.c:1360), e.g. a bearer token.
- A request whose cookie or header is missing, empty or longer than 512 bytes is keyed by its peer IP under the same
  rule, so leaving the session out does not leave a request unlimited.
- Several rules: a request is checked against every rule whose path and method match, in the order they were added;
  the first rule that refuses answers. A later rule does not consume a token of a request an earlier rule refused.
- Validation throws `InvalidArgumentException` at `addRateLimit`: requests and perSeconds 1..10^6, burst 1..10^6,
  paths start with `/` (at most 32 of them), methods are tokens, cookie and header names are tokens, maxKeys
  1024..2^24, at most 16 rules. Config is locked after `start()` as every setter (config_check_locked).

## Algorithm and memory

Token bucket per key, the arithmetic of `ws_publish_allowed` (ws_session.c:877-915): credit in nanoseconds,
`cost = perSeconds·1e9 / requests`, `cap = cost·burst`, refilled from `http_now_coarse_ns()`. `Retry-After` is
`ceil((cost - credit) / 1e9)`, at least 1.

Each rule owns a fixed table, allocated once at `start()`: `maxKeys` slots of `{uint64 tag; uint64 credit_ns;
uint64 stamp_ns}` (24 bytes; 1.5 MiB at the default). A key hashes with keyed SipHash (a random key per server, so a
client cannot aim collisions) to 64 bits; the slot is `hash mod maxKeys` with 4-way probing; the tag is the hash.

- Free slot or tag match: use it.
- All 4 probed slots taken by other tags: take the one whose bucket would now be full (idle at least `cap`); its
  key starts again from a full bucket, which is what it would have anyway. If none is idle, take the slot with the
  oldest stamp. Churning keys can therefore reset an active key's bucket; the reset key gains at most one burst. The
  cost of the churn falls on the churner, who is limited under each key it uses. `requests_rate_limit_evictions_total`
  counts these takeovers.
- No allocation on the request path, no locks: one table per rule per server thread (P2.1). With `setWorkers(N)`
  each worker has its own tables, so a key can get up to N times the limit when its requests land on different
  workers. USAGE.md says so; exact global limits would need shared state, which P2.1 forbids.

## Where it runs

`http_request_prepare_dispatch` (http_protocol_handlers.c:23) is called by every protocol before the static
handler and the spawn (H1 http_connection.c:2897, H2 http2_strategy.c:292, H3 http3_dispatch.c:713, worker
worker_dispatch.c:1320). It returns whether the request may proceed; on refusal it fills the response with
`http_response_set_error(429)` plus `Retry-After` and sets the existing skip flag of the site, so the answer goes out
through the normal response path and keeps the connection (as the decode-failure precedent,
http_connection.c:3104). The H1 WebSocket upgrade bypass (http_connection.c:178) and the H2 CONNECT bypass
(http2_strategy.c:239) run before that point; the check is called there too, so an upgrade is limited like any
request. Path matching uses `uri` up to `?` (the lazy `path` is not computed for it).

## Observability

`requests_rate_limited_total` (SUM) next to `requests_shed_total` (php_http_server.h:908), in `getTelemetry()`
(http_server_class.c:5615) and its contract test; `requests_rate_limit_evictions_total` likewise.

## Out of scope

- A trusted proxy's client address (`X-Forwarded-For`, PROXY protocol): keyed by peer IP until the server knows
  which proxies to trust. `keyByHeader('x-forwarded-for')` is spoofable and the docs say so.
- Limits per account computed in PHP (a session resolved to a user): the cookie value is the session, which is what
  the server can see.
- Exact limits across worker threads.

## Tests

- cmocka `tests/unit/core/test_rate_limit.c`: bucket arithmetic and Retry-After, refill after the window, burst,
  probing and takeover of idle and oldest slots, cookie parsing (several cookies, quoted values, empty, oversize).
- phpt `tests/phpt/server/core/0xx-rate-limit-*.phpt`: config validation; H1 over the limit answers 429 with
  Retry-After without running the handler and keeps the connection; recovery after the window; key by cookie and by
  header, with fallback to the peer IP; path and method filters; telemetry counter; H2 and WebSocket upgrade refused.
