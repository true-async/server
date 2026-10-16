--TEST--
Rate limit (#414): RateLimit, addRateLimit() and setTrustedProxies() refuse bad values and changes after they lock
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\RateLimit;
use function Async\spawn;
use function Async\delay;

require_once __DIR__ . '/../_free_port.inc';

function attempt(string $label, callable $fn): void
{
    try {
        $fn();
        echo "$label: ok\n";
    } catch (Throwable $e) {
        echo "$label: ", (new ReflectionClass($e))->getShortName(), ": ", $e->getMessage(), "\n";
    }
}

attempt('requests 0', fn() => new RateLimit(0, 1));
attempt('requests 10^6+1', fn() => new RateLimit(1000001, 1));
attempt('perSeconds 0', fn() => new RateLimit(1, 0));
attempt('perSeconds 86401', fn() => new RateLimit(1, 86401));
attempt('bounds', fn() => new RateLimit(1000000, 86400));
attempt('burst 0', fn() => (new RateLimit(1, 1))->burst(0));
attempt('burst past 2^62 ns', fn() => (new RateLimit(1, 86400))->burst(53376));
attempt('burst at 2^62 ns', fn() => (new RateLimit(1, 86400))->burst(53375));
attempt('path without slash', fn() => (new RateLimit(1, 1))->paths('api'));
attempt('path with trailing slash', fn() => (new RateLimit(1, 1))->paths('/api/'));
attempt('path root', fn() => (new RateLimit(1, 1))->paths('/'));
attempt('no paths', fn() => (new RateLimit(1, 1))->paths());
attempt('33 paths', fn() => (new RateLimit(1, 1))->paths(...array_map(fn($i) => "/p$i", range(1, 33))));
attempt('path not a string', fn() => (new RateLimit(1, 1))->paths('/a', 5));
attempt('method with a space', fn() => (new RateLimit(1, 1))->methods('GET POST'));
attempt('methods', fn() => (new RateLimit(1, 1))->methods('get', 'PROPFIND'));
attempt('maxKeys 1023', fn() => (new RateLimit(1, 1))->maxKeys(1023));
attempt('maxKeys 2^20+1', fn() => (new RateLimit(1, 1))->maxKeys((1 << 20) + 1));
attempt('ipv6Prefix 47', fn() => (new RateLimit(1, 1))->ipv6Prefix(47));
attempt('ipv6Prefix 129', fn() => (new RateLimit(1, 1))->ipv6Prefix(129));
attempt('name with a space', fn() => (new RateLimit(1, 1))->name('a b'));
attempt('name empty', fn() => (new RateLimit(1, 1))->name(''));

$config = new HttpServerConfig();
attempt('trusted: bad entry', fn() => $config->setTrustedProxies(['10.0.0.0/8', 'proxy.example']));
echo 'trusted after a refused list: ', json_encode($config->getTrustedProxies()), "\n";
attempt('trusted: not a string', fn() => $config->setTrustedProxies([10]));
attempt('trusted: 65 entries', fn() => $config->setTrustedProxies(array_fill(0, 65, '10.0.0.1')));
attempt('trusted: short mapped block', fn() => $config->setTrustedProxies(['::ffff:0:0/95']));
attempt('trusted: 64 entries', fn() => $config->setTrustedProxies(array_fill(0, 64, '10.0.0.1')));
attempt('trusted: v6 and unix', fn() => $config->setTrustedProxies(['2001:db8::1/48', 'unix', '::1']));
echo 'trusted: ', json_encode($config->getTrustedProxies()), "\n";

$port = tas_free_port();
$config->addListener('127.0.0.1', $port);
$server = new HttpServer($config);
attempt('trusted after new HttpServer', fn() => $config->setTrustedProxies([]));

$rule = (new RateLimit(5, 1))->name('api');
attempt('add', fn() => $server->addRateLimit($rule));
attempt('setter after add', fn() => $rule->burst(2));
attempt('name after add', fn() => $rule->name('other'));
attempt('same name again', fn() => $server->addRateLimit((new RateLimit(1, 1))->name('api')));
attempt('default name', fn() => $server->addRateLimit(new RateLimit(1, 1)));
attempt('past 2^21 buckets', fn() => $server->addRateLimit((new RateLimit(1, 1))->maxKeys(1 << 20)->name('big1'))
                                          ->addRateLimit((new RateLimit(1, 1))->maxKeys(1 << 20)->name('big2')));

$server->addHttpHandler(fn($req, $res) => $res->setBody('ok'));

spawn(function () use ($server) {
    delay(50);
    attempt('add while running', fn() => $server->addRateLimit(new RateLimit(1, 1)));
    echo 'rules: ', json_encode(array_keys($server->getTelemetry()['rate_limits'])), "\n";
    $server->stop();
});

$server->start();
echo "done\n";
--EXPECT--
requests 0: HttpServerInvalidArgumentException: RateLimit: requests must be 1 to 1000000, 0 given
requests 10^6+1: HttpServerInvalidArgumentException: RateLimit: requests must be 1 to 1000000, 1000001 given
perSeconds 0: HttpServerInvalidArgumentException: RateLimit: perSeconds must be 1 to 86400, 0 given
perSeconds 86401: HttpServerInvalidArgumentException: RateLimit: perSeconds must be 1 to 86400, 86401 given
bounds: ok
burst 0: HttpServerInvalidArgumentException: RateLimit::burst(): must be 1 to 1000000, 0 given
burst past 2^62 ns: HttpServerInvalidArgumentException: RateLimit: a bucket of 53376 requests at 1 per 86400 s holds more than 2^62 ns; lower the burst
burst at 2^62 ns: ok
path without slash: HttpServerInvalidArgumentException: RateLimit::paths(): "api" must start with "/", not end with one unless it is "/", and be at most 1024 bytes
path with trailing slash: HttpServerInvalidArgumentException: RateLimit::paths(): "/api/" must start with "/", not end with one unless it is "/", and be at most 1024 bytes
path root: ok
no paths: ArgumentCountError: TrueAsync\RateLimit::paths() expects at least 1 argument, 0 given
33 paths: HttpServerInvalidArgumentException: RateLimit::paths(): 1 to 32 prefixes, 33 given
path not a string: TypeError: TrueAsync\RateLimit::paths(): Argument #2 must be of type string, int given
method with a space: HttpServerInvalidArgumentException: RateLimit::methods(): "GET POST" is not a method token
methods: ok
maxKeys 1023: HttpServerInvalidArgumentException: RateLimit::maxKeys(): must be 1024 to 1048576, 1023 given
maxKeys 2^20+1: HttpServerInvalidArgumentException: RateLimit::maxKeys(): must be 1024 to 1048576, 1048577 given
ipv6Prefix 47: HttpServerInvalidArgumentException: RateLimit::ipv6Prefix(): must be 48 to 128, 47 given
ipv6Prefix 129: HttpServerInvalidArgumentException: RateLimit::ipv6Prefix(): must be 48 to 128, 129 given
name with a space: HttpServerInvalidArgumentException: RateLimit::name(): 1 to 64 bytes of letters, digits, "_", "-" and "."
name empty: HttpServerInvalidArgumentException: RateLimit::name(): 1 to 64 bytes of letters, digits, "_", "-" and "."
trusted: bad entry: HttpServerInvalidArgumentException: setTrustedProxies(): "proxy.example" is not an IPv4 or IPv6 address or block, or "unix"
trusted after a refused list: []
trusted: not a string: HttpServerInvalidArgumentException: setTrustedProxies(): each entry must be a string, int given
trusted: 65 entries: HttpServerInvalidArgumentException: setTrustedProxies(): at most 64 entries
trusted: short mapped block: HttpServerInvalidArgumentException: setTrustedProxies(): "::ffff:0:0/95" is not an IPv4 or IPv6 address or block, or "unix"
trusted: 64 entries: ok
trusted: v6 and unix: ok
trusted: ["2001:db8::\/48","::1\/128","unix"]
trusted after new HttpServer: HttpServerRuntimeException: Cannot modify HttpServerConfig after server has started
add: ok
setter after add: HttpServerRuntimeException: Cannot modify a RateLimit after it was added to a server
name after add: HttpServerRuntimeException: Cannot modify a RateLimit after it was added to a server
same name again: HttpServerInvalidArgumentException: addRateLimit(): the server already has a rule named "api"
default name: ok
past 2^21 buckets: HttpServerInvalidArgumentException: addRateLimit(): the server's rules would keep 2228224 buckets, more than 2097152; lower maxKeys()
add while running: HttpServerRuntimeException: Cannot add a rate limit while server is running
rules: ["api","rule2","big1"]
done
