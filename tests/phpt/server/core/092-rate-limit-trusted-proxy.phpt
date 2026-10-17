--TEST--
Rate limit (#414): behind a trusted proxy X-Forwarded-For names the client; from any other peer it is ignored
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
if (PHP_OS_FAMILY !== 'Linux') die('skip needs 127.0.0.2 as a source address (Linux loopback)');
?>
--FILE--
<?php
/* 127.0.0.1 is a trusted proxy and 127.0.0.2 is not. Through the proxy each
 * forwarded client has its own bucket, the trusted hops on the right are
 * skipped, and an element that is not an address stops the walk at the proxy
 * itself. From 127.0.0.2 the header is ignored, so a client cannot dodge the
 * limit by inventing addresses. getRemoteAddress() stays the socket peer. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\RateLimit;
use function Async\spawn;
use function Async\delay;

require_once __DIR__ . '/../_free_port.inc';
require_once __DIR__ . '/_rate_limit.inc';

$port = tas_free_port();
$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setTrustedProxies(['127.0.0.1', '::ffff:10.0.0.0/104', 'unix'])
    ->setReadTimeout(10)
    ->setWriteTimeout(10);

echo "trusted: ", implode(' ', $config->getTrustedProxies()), "\n";

$server = new HttpServer($config);
$server->addRateLimit(new RateLimit(1, 60));
$server->addHttpHandler(fn($req, $res) => $res->setBody('peer ' . $req->getRemoteAddress()));

function from(string $source, int $port, ?string $forwarded): string
{
    $ctx = stream_context_create(['socket' => ['bindto' => "$source:0"]]);
    $fp  = stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 2,
                                STREAM_CLIENT_CONNECT, $ctx);
    stream_set_timeout($fp, 5);
    $out = rl_describe(rl_get($fp, '/', $forwarded === null ? [] : ['X-Forwarded-For' => $forwarded]));
    fclose($fp);
    return $out;
}

spawn(function () use ($server, $port) {
    delay(50);
    echo "via proxy, client A: ", from('127.0.0.1', $port, '203.0.113.1'), "\n";
    echo "via proxy, client A again: ", from('127.0.0.1', $port, '203.0.113.1'), "\n";
    echo "via proxy, client B: ", from('127.0.0.1', $port, '203.0.113.2'), "\n";
    echo "client A behind two trusted hops: ", from('127.0.0.1', $port, '203.0.113.1, 10.1.2.3'), "\n";
    /* A client-written address on the left is not the client: the walk goes
     * from the right, so this is client A again, not 198.51.100.99. */
    echo "client A with a forged address before it: ", from('127.0.0.1', $port, '198.51.100.99, 203.0.113.1'), "\n";
    echo "client C with a port: ", from('127.0.0.1', $port, '198.51.100.7:4711'), "\n";
    echo "unknown element, keyed as the proxy: ", from('127.0.0.1', $port, 'unknown'), "\n";
    echo "the proxy itself again: ", from('127.0.0.1', $port, null), "\n";

    /* Not a trusted peer: the header changes nothing. */
    echo "spoofer, first: ", from('127.0.0.2', $port, '192.0.2.50'), "\n";
    echo "spoofer, new address: ", from('127.0.0.2', $port, '192.0.2.51'), "\n";
    $server->stop();
});

$server->start();
echo "done\n";
--EXPECT--
trusted: 127.0.0.1/32 10.0.0.0/8 unix
via proxy, client A: 200 body=peer 127.0.0.1
via proxy, client A again: 429 retry-after=60 body=Too Many Requests
via proxy, client B: 200 body=peer 127.0.0.1
client A behind two trusted hops: 429 retry-after=60 body=Too Many Requests
client A with a forged address before it: 429 retry-after=60 body=Too Many Requests
client C with a port: 200 body=peer 127.0.0.1
unknown element, keyed as the proxy: 200 body=peer 127.0.0.1
the proxy itself again: 429 retry-after=60 body=Too Many Requests
spoofer, first: 200 body=peer 127.0.0.2
spoofer, new address: 429 retry-after=60 body=Too Many Requests
done
