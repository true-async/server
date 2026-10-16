--TEST--
Rate limit (#414): an HTTP/1 WebSocket upgrade over the limit gets 429 Too Many Requests with Retry-After, and is counted
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
if (!class_exists('TrueAsync\\WebSocket')) die('skip needs WebSocket support');
?>
--FILE--
<?php
/* The upgrade short-circuits the dispatch site, so it runs the check itself
 * once the handshake is valid: a refused one is answered 429 with its reason
 * phrase and Retry-After, the connection closes as on every refused upgrade,
 * the WebSocket handler does not run, and the refusal counts in the request
 * and 4xx totals. An invalid handshake is still answered as before. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\RateLimit;
use TrueAsync\WebSocket;
use function Async\spawn;
use function Async\delay;

require_once __DIR__ . '/../_free_port.inc';

$port = tas_free_port();
$server = new HttpServer((new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setStatsEnabled(true)
    ->setReadTimeout(10)
    ->setWriteTimeout(10)
    ->setWsPingIntervalMs(0));

$server->addRateLimit(new RateLimit(1, 60));

$opened = 0;
$server->addWebSocketHandler(function (WebSocket $ws) use (&$opened) {
    $opened++;
    $ws->close();
});
$server->addHttpHandler(fn($req, $res) => $res->setBody('ok'));

function upgrade(int $port, string $version = '13'): string
{
    $fp = stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 2);
    stream_set_timeout($fp, 5);
    fwrite($fp, "GET /chat HTTP/1.1\r\nHost: t\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
              . "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: $version\r\n\r\n");
    $head = '';

    while (!str_contains($head, "\r\n\r\n") && ($line = fgets($fp)) !== false) {
        $head .= $line;
    }

    $lines = explode("\r\n", trim($head));
    $out = $lines[0];

    foreach ($lines as $line) {
        if (stripos($line, 'retry-after:') === 0 || stripos($line, 'connection:') === 0) {
            $out .= ' | ' . strtolower($line);
        }
    }

    if (str_starts_with($lines[0], 'HTTP/1.1 4')) {
        stream_get_contents($fp);
        $out .= ' | closed=' . (feof($fp) ? 'yes' : 'no');
    }

    fclose($fp);
    return $out;
}

spawn(function () use ($server, $port, &$opened) {
    delay(50);
    echo "first: ", upgrade($port), "\n";
    echo "second: ", upgrade($port), "\n";
    echo "bad version: ", upgrade($port, '8'), "\n";
    delay(100);
    echo "handler opened=$opened\n";

    $totals = $server->getStats()['totals'];
    echo "rate_limited=", $totals['requests_rate_limited_total'],
         " 4xx=", $totals['responses_4xx_total'], "\n";
    $server->stop();
});

$server->start();
echo "done\n";
--EXPECT--
first: HTTP/1.1 101 Switching Protocols | connection: upgrade
second: HTTP/1.1 429 Too Many Requests | connection: close | retry-after: 60 | closed=yes
bad version: HTTP/1.1 426 Upgrade Required | connection: close | closed=yes
handler opened=1
rate_limited=1 4xx=1
done
