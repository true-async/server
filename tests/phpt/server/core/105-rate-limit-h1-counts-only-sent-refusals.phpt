--TEST--
Rate limit (#414): HTTP/1 counts a refusal only when the 429 is sent; a 413 or a 405 upgrade answer leaves the counts and the token alone
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
/* A POST over the limit is judged when its headers end, but its answer is a
 * 413 when the body breaks setMaxBodySize(), declared or chunked, and a POST
 * carrying Upgrade: websocket is answered 405 by the upgrade path. None of
 * those is a 429, so none counts as rate limited, and neither the 405 nor a
 * 413 for a declared length spends a token: the POST after them is still
 * admitted. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\RateLimit;
use TrueAsync\WebSocket;
use function Async\spawn;
use function Async\delay;

require_once __DIR__ . '/../_free_port.inc';
require_once __DIR__ . '/_rate_limit.inc';

$port = tas_free_port();
$server = new HttpServer((new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setMaxBodySize(1024)
    ->setReadTimeout(10)
    ->setWriteTimeout(10)
    ->setWsPingIntervalMs(0));

$server->addRateLimit((new RateLimit(1, 60))->methods('POST')->name('posts'));
$server->addHttpHandler(fn($req, $res) => $res->setBody('ok'));
$server->addWebSocketHandler(fn(WebSocket $ws) => $ws->close());

function counts(HttpServer $server): string
{
    $t = $server->getTelemetry();
    return 'limited=' . $t['requests_rate_limited_total'] . ' rule=' . $t['rate_limits']['posts']['limited'];
}

function status_of(int $port, string $request): string
{
    $fp = rl_connect($port);
    fwrite($fp, $request);
    $r = rl_read_response($fp);
    fclose($fp);
    return $r === null ? 'closed' : (string) $r['status'];
}

spawn(function () use ($server, $port) {
    delay(50);
    $upgrade = "POST /ws HTTP/1.1\r\nHost: t\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
             . "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n"
             . "Content-Length: 0\r\n\r\n";
    $post = "POST /p HTTP/1.1\r\nHost: t\r\nContent-Length: 2\r\n\r\nhi";

    /* The client's first request: a declared body past the limit is answered
     * 413 before any verdict, so the POST below still has the token. */
    echo "first request, declared body past the limit: ", status_of($port,
        "POST /p HTTP/1.1\r\nHost: t\r\nContent-Length: 4096\r\n\r\n"), "\n";
    echo "upgrade POST #1: ", status_of($port, $upgrade), "\n";
    echo "upgrade POST #2: ", status_of($port, $upgrade), "\n";
    echo "POST: ", status_of($port, $post), "\n";
    echo counts($server), "\n";

    echo "declared body past the limit: ", status_of($port,
        "POST /p HTTP/1.1\r\nHost: t\r\nContent-Length: 4096\r\n\r\n"), "\n";
    echo "chunked body past the limit: ", status_of($port,
        "POST /p HTTP/1.1\r\nHost: t\r\nTransfer-Encoding: chunked\r\n\r\n800\r\n"
        . str_repeat('x', 2048) . "\r\n0\r\n\r\n"), "\n";
    echo counts($server), "\n";

    echo "POST over the limit: ", status_of($port, $post), "\n";
    echo counts($server), "\n";
    $server->stop();
});

$server->start();
echo "done\n";
--EXPECT--
first request, declared body past the limit: 413
upgrade POST #1: 405
upgrade POST #2: 405
POST: 200
limited=0 rule=0
declared body past the limit: 413
chunked body past the limit: 413
limited=0 rule=0
POST over the limit: 429
limited=1 rule=1
done
