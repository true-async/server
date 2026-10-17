--TEST--
Rate limit (#414): path rules match request targets on both sides of the 1024-byte stack scratch
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
/* A target up to 1024 bytes is decoded on the stack, a longer one in a heap
 * buffer of 2 * length + 2. Both must decode, normalise and match: one
 * request a minute under /api, from one client. Under valgrind (run-tests -m)
 * a heap buffer sized short is an invalid write. */

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

$server->addRateLimit((new RateLimit(1, 60))->paths('/api'));
$server->addHttpHandler(fn($req, $res) => $res->setBody('len ' . strlen($req->getUri())));

function target(string $head, int $length): string
{
    return $head . str_repeat('a', $length - strlen($head));
}

spawn(function () use ($server, $port) {
    delay(50);
    $fp = rl_connect($port);

    echo "1024 under /api: ", rl_describe(rl_get($fp, target('/api/', 1024))), "\n";
    echo "1025 under /api: ", rl_describe(rl_get($fp, target('/api/', 1025))), "\n";
    echo "1025 elsewhere: ", rl_describe(rl_get($fp, target('/other/', 1025))), "\n";
    echo "4000 dotted and encoded: ", rl_describe(rl_get($fp, target('//x/../%61pi/', 4000))), "\n";
    echo "1024 elsewhere: ", rl_describe(rl_get($fp, target('/apix/', 1024))), "\n";
    fclose($fp);
    $server->stop();
});

$server->start();
echo "done\n";
--EXPECT--
1024 under /api: 200 body=len 1024
1025 under /api: 429 retry-after=60 body=Too Many Requests
1025 elsewhere: 200 body=len 1025
4000 dotted and encoded: 429 retry-after=60 body=Too Many Requests
1024 elsewhere: 200 body=len 1024
done
