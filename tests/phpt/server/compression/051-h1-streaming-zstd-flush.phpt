--TEST--
Compression H1 streaming: a zstd chunk reaches the client before the stream ends (#170)
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
if (!class_exists('TrueAsync\HttpServerConfig')) die('skip http_server not loaded');
if (!in_array('zstd', TrueAsync\HttpServerConfig::getSupportedEncodings(), true)) {
    die('skip zstd backend not built (configure with libzstd-dev)');
}
?>
--FILE--
<?php
use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;
use function Async\delay;

require_once __DIR__ . '/../_free_port.inc';
require_once __DIR__ . '/_flush_probe.inc';

$port = tas_free_port();
$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(5)
    ->setWriteTimeout(5)
    ->setRequestMaxDecompressedSize(256 * 1024);

$server = new HttpServer($config);

$head = str_repeat("progressive zstd chunk\n", 400);
$tail = str_repeat("tail written after the gate\n", 20);
$gate = false;

/* PHP has no zstd decoder, so the client checks two things it can
 * check: that a prefix arrived while the handler was still parked,
 * and that the server's own request decoder accepts the whole flushed
 * stream. Without a flush this encoder emits nothing at all mid-stream
 * (measured: 0 bytes), so any byte here comes from a flushed block
 * rather than from a stream header. */
$server->addHttpHandler(function ($req, $resp) use (&$gate, $head, $tail) {
    if ($req->getPath() === '/release') {
        $gate = true;
        $resp->setHeader('Content-Type', 'text/plain')->setBody('ok')->end();
        return;
    }

    if ($req->getPath() === '/echo') {
        $resp->setHeader('Content-Type', 'text/plain')
             ->setBody('len=' . strlen($req->getBody()))
             ->end();
        return;
    }

    $resp->setHeader('Content-Type', 'text/html');
    $resp->send($head);

    while (!$gate) {
        delay(10);
    }

    $resp->end($tail);
});

$client = spawn(function () use ($port, $server, $head, $tail) {
    delay(20);

    [$h, $early, $whole] = tas_flush_probe($port, 'zstd');

    echo "content-encoding: ", $h['content-encoding'] ?? '<none>', "\n";
    echo "early-bytes: ", strlen($early) > 0 ? 'some' : 'none', "\n";

    $raw = tas_h1_fire($port,
        "POST /echo HTTP/1.1\r\nHost: x\r\n"
      . "Content-Length: " . strlen($whole) . "\r\n"
      . "Content-Encoding: zstd\r\nConnection: close\r\n\r\n"
      . $whole);
    [, $body] = tas_h1_split($raw);
    $expected = 'len=' . strlen($head . $tail);
    echo "round-trip: ", ($body === $expected) ? 'ok' : "MISMATCH ($body vs $expected)", "\n";

    delay(50);
    $server->stop();
});

$server->start();
await($client);
echo "Done\n";
?>
--EXPECT--
content-encoding: zstd
early-bytes: some
round-trip: ok
Done
