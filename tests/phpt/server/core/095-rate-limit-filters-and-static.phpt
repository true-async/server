--TEST--
Rate limit (#414): path and method filters, encoded and dot-segment paths, two rules at once, and static files left uncounted
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
/* `works` holds /api/works (decoded and normalised, so %77orks and
 * //api/./works are the same route) to one GET or HEAD a minute; /api/worksheet
 * is not under it. `all` holds every request to four a minute, and a request
 * under both takes a token from `all` only when `works` admits it too. A file
 * a static mount serves is not counted; one it passes on (StaticOnMissing::NEXT)
 * reaches a handler and is. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\RateLimit;
use TrueAsync\StaticHandler;
use TrueAsync\StaticOnMissing;
use function Async\spawn;
use function Async\delay;

require_once __DIR__ . '/../_free_port.inc';
require_once __DIR__ . '/_rate_limit.inc';

$root = sys_get_temp_dir() . '/tas-rl-095-' . getmypid();
@mkdir($root);
file_put_contents("$root/a.txt", "static file\n");

$port = tas_free_port();
$server = new HttpServer((new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(10)
    ->setWriteTimeout(10));

$server->addStaticHandler((new StaticHandler('/static/', $root))->setOnMissing(StaticOnMissing::NEXT));
$server->addRateLimit((new RateLimit(1, 60))->name('works')->paths('/api/works')->methods('get', 'HEAD'));
$server->addRateLimit((new RateLimit(4, 60))->name('all'));
$server->addHttpHandler(fn($req, $res) => $res->setBody('handler ' . $req->getMethod() . ' ' . $req->getUri()));

spawn(function () use ($server, $port, $root) {
    delay(50);
    $fp = rl_connect($port);

    for ($i = 1; $i <= 5; $i++) {
        echo "static hit #$i: ", rl_describe(rl_get($fp, '/static/a.txt')), "\n";
    }

    echo "works: ", rl_describe(rl_get($fp, '/api/works?page=2')), "\n";
    echo "works encoded: ", rl_describe(rl_get($fp, '/api/%77orks')), "\n";
    echo "works dotted: ", rl_describe(rl_get($fp, '//api/./works/7')), "\n";
    echo "works via ..: ", rl_describe(rl_get($fp, '/x/../api/works')), "\n";
    echo "malformed %: ", rl_describe(rl_get($fp, '/other/%zz')), "\n";
    echo "worksheet: ", rl_describe(rl_get($fp, '/api/worksheet')), "\n";

    fwrite($fp, "POST /api/works HTTP/1.1\r\nHost: t\r\nContent-Length: 0\r\n\r\n");
    echo "works POST: ", rl_describe(rl_read_response($fp)), "\n";

    echo "static miss: ", rl_describe(rl_get($fp, '/static/missing.txt')), "\n";
    echo "anything now: ", rl_describe(rl_get($fp, '/x')), "\n";
    echo "static hit after: ", rl_describe(rl_get($fp, '/static/a.txt')), "\n";
    fclose($fp);

    $t = $server->getTelemetry();
    echo "limited=", $t['requests_rate_limited_total'], " rules=", json_encode($t['rate_limits']), "\n";
    $server->stop();
});

$server->start();
@unlink("$root/a.txt");
@rmdir($root);
echo "done\n";
--EXPECT--
static hit #1: 200 body=static file
static hit #2: 200 body=static file
static hit #3: 200 body=static file
static hit #4: 200 body=static file
static hit #5: 200 body=static file
works: 200 body=handler GET /api/works?page=2
works encoded: 429 retry-after=60 body=Too Many Requests
works dotted: 429 retry-after=60 body=Too Many Requests
works via ..: 429 retry-after=60 body=Too Many Requests
malformed %: 429 retry-after=60 body=Too Many Requests
worksheet: 200 body=handler GET /api/worksheet
works POST: 200 body=handler POST /api/works
static miss: 200 body=handler GET /static/missing.txt
anything now: 429 retry-after=15 body=Too Many Requests
static hit after: 200 body=static file
limited=5 rules={"works":{"limited":4,"evictions":0},"all":{"limited":1,"evictions":0}}
done
