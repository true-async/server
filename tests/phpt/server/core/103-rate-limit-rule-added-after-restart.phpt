--TEST--
Rate limit (#414): a rule added between stop() and the next start() gets buckets of its own
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
/* The first run allocates buckets for rule A. B is added after stop(), and
 * the second start() must allocate buckets for it too: with only A's tables,
 * B's check read past them. A keeps its buckets across the restart, so the
 * client it refused is still refused. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\RateLimit;
use function Async\spawn;
use function Async\delay;

require_once __DIR__ . '/../_free_port.inc';
require_once __DIR__ . '/_rate_limit.inc';

$port = tas_free_port();
$server = new HttpServer((new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(10)
    ->setWriteTimeout(10));

$server->addRateLimit((new RateLimit(1, 60))->paths('/a')->name('a'));
$server->addHttpHandler(fn($req, $res) => $res->setBody('ok ' . $req->getPath()));

function run(HttpServer $server, int $port, array $paths, string $label): void
{
    spawn(function () use ($server, $port, $paths, $label) {
        delay(50);
        $fp = rl_connect($port);

        foreach ($paths as $path) {
            echo "$label $path: ", rl_describe(rl_get($fp, $path)), "\n";
        }

        fclose($fp);
        echo "$label rules: ", json_encode(array_keys($server->getTelemetry()['rate_limits'])), "\n";
        $server->stop();
    });

    $server->start();
}

run($server, $port, ['/a', '/a'], 'run 1');

$server->addRateLimit((new RateLimit(1, 60))->paths('/b')->name('b'));

run($server, $port, ['/a', '/b', '/b'], 'run 2');
echo "done\n";
--EXPECT--
run 1 /a: 200 body=ok /a
run 1 /a: 429 retry-after=60 body=Too Many Requests
run 1 rules: ["a"]
run 2 /a: 429 retry-after=60 body=Too Many Requests
run 2 /b: 200 body=ok /b
run 2 /b: 429 retry-after=60 body=Too Many Requests
run 2 rules: ["a","b"]
done
