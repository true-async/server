--TEST--
Rate limit (#414): with more than eight rules a refused POST keeps and counts every rule's refusal
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
/* An HTTP/1 POST is judged before its body, and its per-rule counts wait on
 * the request until the 429 is sent. Up to eight rules they are gathered on
 * the stack; nine take a heap array. Every rule refuses the second POST,
 * so each counts exactly one refusal. */

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

for ($i = 1; $i <= 9; $i++) {
    $server->addRateLimit((new RateLimit(1, 60))->maxKeys(1024)->name("r$i"));
}

$server->addHttpHandler(fn($req, $res) => $res->setBody('ok ' . $req->getBody()));

spawn(function () use ($server, $port) {
    delay(50);
    $fp = rl_connect($port);

    for ($i = 1; $i <= 2; $i++) {
        fwrite($fp, "POST /p HTTP/1.1\r\nHost: t\r\nContent-Length: 2\r\n\r\nhi");
        echo "POST #$i: ", rl_describe(rl_read_response($fp)), "\n";
    }

    fclose($fp);
    $t = $server->getTelemetry();
    echo "limited=", $t['requests_rate_limited_total'], "\n";
    echo "per rule: ", implode(' ', array_map(fn($r) => $r['limited'] . '/' . $r['evictions'], $t['rate_limits'])), "\n";
    $server->stop();
});

$server->start();
echo "done\n";
--EXPECT--
POST #1: 200 body=ok hi
POST #2: 429 retry-after=60 body=Too Many Requests
limited=1
per rule: 1/0 1/0 1/0 1/0 1/0 1/0 1/0 1/0 1/0
done
