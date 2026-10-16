--TEST--
Rate limit (#414): a request a static mount passes on (StaticOnMissing::NEXT) spends a token; a file it serves does not
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
/* One request a minute for everything. Static hits come first and spend
 * nothing; a miss the mount passes to the handler spends the only token, so
 * the next request to the handler is refused, and static files are still
 * served after that. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\RateLimit;
use TrueAsync\StaticHandler;
use TrueAsync\StaticOnMissing;
use function Async\spawn;
use function Async\delay;

require_once __DIR__ . '/../_free_port.inc';
require_once __DIR__ . '/_rate_limit.inc';

$root = sys_get_temp_dir() . '/tas-rl-108-' . getmypid();
@mkdir($root);
file_put_contents("$root/a.txt", "static\n");

$port = tas_free_port();
$server = new HttpServer((new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(10)
    ->setWriteTimeout(10));

$server->addStaticHandler((new StaticHandler('/s/', $root))->setOnMissing(StaticOnMissing::NEXT));
$server->addRateLimit(new RateLimit(1, 60));
$server->addHttpHandler(fn($req, $res) => $res->setBody('handler ' . $req->getPath()));

spawn(function () use ($server, $port) {
    delay(50);
    $fp = rl_connect($port);
    echo "hit: ", rl_describe(rl_get($fp, '/s/a.txt')), "\n";
    echo "hit: ", rl_describe(rl_get($fp, '/s/a.txt')), "\n";
    echo "miss, passed on: ", rl_describe(rl_get($fp, '/s/missing.txt')), "\n";
    echo "handler after it: ", rl_describe(rl_get($fp, '/app')), "\n";
    echo "hit after it: ", rl_describe(rl_get($fp, '/s/a.txt')), "\n";
    fclose($fp);
    $server->stop();
});

$server->start();
@unlink("$root/a.txt");
@rmdir($root);
echo "done\n";
--EXPECT--
hit: 200 body=static
hit: 200 body=static
miss, passed on: 200 body=handler /s/missing.txt
handler after it: 429 retry-after=60 body=Too Many Requests
hit after it: 200 body=static
done
