--TEST--
Compression H1: x-gzip is gzip, in Content-Encoding and in Accept-Encoding
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
if (!class_exists('TrueAsync\HttpServerConfig')) die('skip http_server not loaded');
if (!extension_loaded('zlib')) die('skip zlib required');
?>
--FILE--
<?php
/* RFC 9110 §8.4.1.3: a recipient SHOULD treat "x-gzip" as "gzip". A request
 * body sent as x-gzip reaches the handler decoded, and a client accepting
 * x-gzip gets a gzip response. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;
use function Async\delay;

require_once __DIR__ . '/../_free_port.inc';

$port = tas_free_port();
$server = new HttpServer((new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(5)
    ->setWriteTimeout(5));

$page = str_repeat("Hello, x-gzip!\n", 200);   /* past the 1024-byte threshold */

$server->addHttpHandler(function ($req, $resp) use ($page) {
    $resp->setHeader('Content-Type', 'text/plain');
    $resp->setBody($req->getMethod() === 'POST' ? 'got=' . $req->getBody() : $page);
    $resp->end();
});

function exchange(int $port, string $request): array
{
    $fp = stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 2);
    stream_set_timeout($fp, 2);
    fwrite($fp, $request);
    $raw = '';
    while (!feof($fp)) {
        $c = fread($fp, 8192);
        if ($c === '' || $c === false) break;
        $raw .= $c;
    }
    fclose($fp);
    [$head, $body] = explode("\r\n\r\n", $raw, 2) + ['', ''];
    $encoding = preg_match('/^Content-Encoding:\s*(\S+)/mi', $head, $m) ? strtolower($m[1]) : 'identity';

    return [(int) (explode(' ', $head)[1] ?? 0), $encoding, $body];
}

$client = spawn(function () use ($port, $server, $page) {
    delay(50);

    $sent = gzencode('form payload');
    [$status, , $body] = exchange($port, "POST / HTTP/1.1\r\nHost: x\r\n"
        . "Content-Encoding: x-gzip\r\nContent-Length: " . strlen($sent) . "\r\n"
        . "Connection: close\r\n\r\n" . $sent);
    echo "request x-gzip: $status $body\n";

    [$status, $encoding, $body] = exchange($port, "GET / HTTP/1.1\r\nHost: x\r\n"
        . "Accept-Encoding: x-gzip\r\nConnection: close\r\n\r\n");
    echo "accept x-gzip: $status $encoding ", @gzdecode($body) === $page ? 'decodes' : 'MISMATCH', "\n";

    $server->stop();
});

$server->start();
await($client);
?>
--EXPECT--
request x-gzip: 200 got=form payload
accept x-gzip: 200 gzip decodes
