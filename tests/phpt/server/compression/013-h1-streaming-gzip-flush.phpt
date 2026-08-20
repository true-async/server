--TEST--
Compression H1 streaming: a gzip chunk is decodable before the stream ends (#170)
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
if (!class_exists('TrueAsync\HttpServerConfig')) die('skip http_server not loaded');
if (!function_exists('inflate_init')) die('skip ext/zlib not available for client-side inflate');
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
    ->setWriteTimeout(5);

$server = new HttpServer($config);

$head = str_repeat("progressive gzip chunk\n", 200);
$tail = str_repeat("tail written after the gate\n", 50);
$gate = false;

$server->addHttpHandler(function ($req, $resp) use (&$gate, $head, $tail) {
    if ($req->getPath() === '/release') {
        $gate = true;
        $resp->setHeader('Content-Type', 'text/plain')->setBody('ok')->end();
        return;
    }

    /* Same body, no gate, with two empty sends in front: an empty chunk
     * must cost nothing on the wire, so this encodes byte for byte like
     * the gated route above. */
    if ($req->getPath() === '/empties') {
        $resp->setHeader('Content-Type', 'text/html');
        $resp->write('');
        $resp->write('');
        $resp->write($head);
        $resp->end($tail);
        return;
    }

    $resp->setHeader('Content-Type', 'text/html');
    $resp->write($head);

    while (!$gate) {
        delay(10);
    }

    $resp->end($tail);
});

$client = spawn(function () use ($port, $server, $head, $tail) {
    delay(20);

    [$h, $early, $whole] = tas_flush_probe($port, 'gzip');

    echo "content-encoding: ", $h['content-encoding'] ?? '<none>', "\n";

    $ctx  = inflate_init(ZLIB_ENCODING_GZIP);
    $seen = inflate_add($ctx, $early, ZLIB_SYNC_FLUSH);
    echo "early-decoded: ", ($seen === $head)
        ? 'first chunk'
        : 'got ' . strlen($seen) . ' of ' . strlen($head) . ' bytes', "\n";

    $ctx = inflate_init(ZLIB_ENCODING_GZIP);
    echo "round-trip: ", (inflate_add($ctx, $whole, ZLIB_FINISH) === $head . $tail) ? 'ok' : 'MISMATCH', "\n";

    [, $with_empties] = tas_h1_split(tas_h1_fire($port,
        "GET /empties HTTP/1.1\r\nHost: x\r\n"
      . "Accept-Encoding: gzip\r\nConnection: close\r\n\r\n"));
    echo "empty-chunks-cost: ", strlen($with_empties) - strlen($whole), " bytes\n";

    delay(50);
    $server->stop();
});

$server->start();
await($client);
echo "Done\n";
?>
--EXPECT--
content-encoding: gzip
early-decoded: first chunk
round-trip: ok
empty-chunks-cost: 0 bytes
Done
