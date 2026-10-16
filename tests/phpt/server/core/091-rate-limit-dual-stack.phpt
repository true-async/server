--TEST--
Rate limit (#414): IPv4 clients of a dual-stack [::] listener are keyed per address, IPv6 by its /64
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
if (PHP_OS_FAMILY !== 'Linux') die('skip needs 127.0.0.2 as a source address (Linux loopback)');
$s = @stream_socket_server('tcp://[::1]:0', $e, $m);
if ($s === false) die('skip no IPv6 loopback');
?>
--FILE--
<?php
/* A dual-stack listener reports an IPv4 peer as ::ffff:a.b.c.d. Keyed as
 * IPv6 by its /64, every IPv4 client would share one bucket; un-mapped, each
 * address has its own. ::1 is IPv6 and a key of its own. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\RateLimit;
use function Async\spawn;
use function Async\delay;

require_once __DIR__ . '/../_free_port.inc';
require_once __DIR__ . '/_rate_limit.inc';

$port = tas_free_port();
$server = new HttpServer((new HttpServerConfig())
    ->addListener('::', $port)
    ->setReadTimeout(10)
    ->setWriteTimeout(10));

$server->addRateLimit(new RateLimit(1, 60));
$server->addHttpHandler(fn($req, $res) => $res->setBody('from ' . $req->getRemoteAddress()));

/* One request from $source, on a connection of its own. */
function from(string $source, string $target, int $port): string
{
    $host = str_contains($target, ':') ? "[$target]" : $target;
    $bind = str_contains($source, ':') ? "[$source]:0" : "$source:0";
    $ctx  = stream_context_create(['socket' => ['bindto' => $bind]]);
    $fp   = stream_socket_client("tcp://$host:$port", $errno, $errstr, 2,
                                 STREAM_CLIENT_CONNECT, $ctx);

    if ($fp === false) {
        return "connect failed: $errstr";
    }

    stream_set_timeout($fp, 5);
    $out = rl_describe(rl_get($fp));
    fclose($fp);
    return $out;
}

spawn(function () use ($server, $port) {
    delay(50);
    echo "127.0.0.1: ", from('127.0.0.1', '127.0.0.1', $port), "\n";
    echo "127.0.0.1 again: ", from('127.0.0.1', '127.0.0.1', $port), "\n";
    echo "127.0.0.2: ", from('127.0.0.2', '127.0.0.1', $port), "\n";
    echo "::1: ", from('::1', '::1', $port), "\n";
    echo "::1 again: ", from('::1', '::1', $port), "\n";
    $server->stop();
});

$server->start();
echo "done\n";
--EXPECTF--
127.0.0.1: 200 body=from %s127.0.0.1
127.0.0.1 again: 429 retry-after=60 body=Too Many Requests
127.0.0.2: 200 body=from %s127.0.0.2
::1: 200 body=from ::1
::1 again: 429 retry-after=60 body=Too Many Requests
done
