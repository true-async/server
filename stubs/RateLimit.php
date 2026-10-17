<?php

/**
 * @generate-class-entries
 */

namespace TrueAsync;

/**
 * A request rate limit rule (#414): a token bucket per client address.
 *
 * Attach with {@see HttpServer::addRateLimit()}. A request over the limit of
 * any rule it matches is answered `429 Too Many Requests` with `Retry-After`,
 * and the handler does not run; a gRPC call gets `grpc-status: 8`
 * (RESOURCE_EXHAUSTED) instead. A request is checked against every rule it
 * matches first, and takes a token from each only when all of them admit it.
 *
 * The client is the socket peer, or, behind a proxy listed in
 * {@see HttpServerConfig::setTrustedProxies()}, the address the proxies put in
 * `X-Forwarded-For`. IPv6 clients are keyed by their /64 unless
 * {@see RateLimit::ipv6Prefix()} says otherwise.
 *
 * Each server instance keeps its own buckets: with `setWorkers(N)` a client
 * may get up to N times the limit.
 *
 * The rule is locked once added to a server; its setters throw from then on.
 *
 * @strict-properties
 */
final class RateLimit
{
    /**
     * @param int $requests   tokens refilled per window, 1 to 1000000
     * @param int $perSeconds window length in seconds, 1 to 86400
     *
     * @throws HttpServerInvalidArgumentException on a value out of range
     */
    public function __construct(int $requests, int $perSeconds) {}

    /**
     * Bucket size: how many requests a client may make at once after being
     * idle. Default: `$requests`. 1 to 1000000, and the bucket may hold at
     * most 2^62 ns of credit (`perSeconds * burst / requests` seconds).
     *
     * @throws HttpServerInvalidArgumentException
     * @throws HttpServerRuntimeException once the rule is added to a server
     */
    public function burst(int $burst): static {}

    /**
     * Limits the rule to these path prefixes; replaces any earlier list. A
     * prefix starts with `/` and does not end with one, except `/` itself,
     * which means every path: with `/` in the list the other prefixes are
     * dropped, as if paths() had not been called. `/api` covers `/api` and
     * `/api/...`, not `/apix`. The path is matched decoded (`%77` is `w`) and
     * normalised (`//` collapsed, `.` and `..` resolved); a target with a
     * malformed `%` sequence matches every prefix. Up to 32 prefixes.
     * Default: every path.
     *
     * @throws HttpServerInvalidArgumentException
     * @throws HttpServerRuntimeException once the rule is added to a server
     */
    public function paths(string ...$prefixes): static {}

    /**
     * Limits the rule to these request methods; replaces any earlier list.
     * Tokens are upper-cased; HEAD is its own method. Up to 32 methods.
     * Default: every method.
     *
     * @throws HttpServerInvalidArgumentException
     * @throws HttpServerRuntimeException once the rule is added to a server
     */
    public function methods(string ...$methods): static {}

    /**
     * Buckets this rule keeps per server instance, 1024 to 1048576. Default
     * 65536, 1.5 MiB. When the table is full a new client takes the slot of
     * one whose bucket has refilled, else of the one idle longest; that client
     * starts again with a full bucket. The sum over a server's rules is at
     * most 2097152.
     *
     * @throws HttpServerInvalidArgumentException
     * @throws HttpServerRuntimeException once the rule is added to a server
     */
    public function maxKeys(int $maxKeys): static {}

    /**
     * How many leading bits of an IPv6 client form its key, 48 to 128.
     * Default 64: one /64 is one client.
     *
     * @throws HttpServerInvalidArgumentException
     * @throws HttpServerRuntimeException once the rule is added to a server
     */
    public function ipv6Prefix(int $bits): static {}

    /**
     * The rule's name in {@see HttpServer::getTelemetry()}: 1 to 64 bytes of
     * letters, digits, `_`, `-` and `.`. Default `rule<N>`, N its position
     * among the server's rules from 1. Names are unique per server.
     *
     * @throws HttpServerInvalidArgumentException
     * @throws HttpServerRuntimeException once the rule is added to a server
     */
    public function name(string $name): static {}
}
