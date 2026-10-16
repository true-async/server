--TEST--
Rate limit (#414): HTTP/1 answers 429 with Retry-After, runs no handler, keeps the connection, and recovers
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
/* Two requests a minute: the third on the same connection is refused with
 * Retry-After 30 (one token is 30 s), the handler does not run, and the
 * connection keeps serving refusals. A second rule of one request a second
 * shows the bucket refilling: refused, then admitted a second later. */

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

$server->addRateLimit((new RateLimit(2, 60))->name('slow')->paths('/slow'));
$server->addRateLimit((new RateLimit(1, 1))->name('fast')->paths('/fast'));

$handled = 0;
$server->addHttpHandler(function ($req, $res) use (&$handled) {
    $handled++;
    $res->setBody('ok ' . $req->getPath());
});

spawn(function () use ($server, $port, &$handled) {
    delay(50);
    $fp = rl_connect($port);

    for ($i = 1; $i <= 4; $i++) {
        echo "slow #$i: ", rl_describe(rl_get($fp, '/slow')), "\n";
    }

    echo "handled=$handled\n";

    /* HEAD carries the status and Retry-After without a body. */
    fwrite($fp, "HEAD /slow HTTP/1.1\r\nHost: t\r\n\r\n");
    echo "head: ", rl_describe(rl_read_response($fp, true)), "\n";

    /* Another path is under no rule. */
    echo "free: ", rl_describe(rl_get($fp, '/other')), "\n";

    echo "fast #1: ", rl_describe(rl_get($fp, '/fast')), "\n";
    echo "fast #2: ", rl_describe(rl_get($fp, '/fast')), "\n";
    delay(1100);
    echo "fast after 1.1 s: ", rl_describe(rl_get($fp, '/fast')), "\n";

    fclose($fp);

    $t = $server->getTelemetry();
    echo "rate_limited_total=", $t['requests_rate_limited_total'], "\n";
    echo "rules=", json_encode($t['rate_limits']), "\n";
    $server->stop();
});

$server->start();
echo "done\n";
--EXPECT--
slow #1: 200 body=ok /slow
slow #2: 200 body=ok /slow
slow #3: 429 retry-after=30 body=Too Many Requests
slow #4: 429 retry-after=30 body=Too Many Requests
handled=2
head: 429 retry-after=30 body=
free: 200 body=ok /other
fast #1: 200 body=ok /fast
fast #2: 429 retry-after=1 body=Too Many Requests
fast after 1.1 s: 200 body=ok /fast
rate_limited_total=4
rules={"slow":{"limited":3,"evictions":0},"fast":{"limited":1,"evictions":0}}
done
