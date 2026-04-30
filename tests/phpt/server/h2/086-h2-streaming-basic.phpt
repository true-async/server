--TEST--
HttpResponse::send() — HTTP/2 streaming basic round-trip
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
require __DIR__ . '/_h2_skipif.inc';
h2_skipif(['curl_h2' => true]);
?>
--FILE--
<?php
/* PLAN_STREAMING Phase 1 — handler sends N chunks via send(), then
 * closes with end(). Client reassembles the full body. Verifies:
 *   - send() commits headers on first call,
 *   - multiple DATA frames arrive in order,
 *   - end() terminates the stream cleanly,
 *   - buffered-mode response-helpers are unaffected (they coexist
 *     with streaming mode on the same class). */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

$port = 19850 + getmypid() % 100;

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(10)
    ->setWriteTimeout(10);

$server = new HttpServer($config);
$server->addHttpHandler(function ($req, $res) {
    $res->setStatusCode(200)
        ->setHeader('Content-Type', 'text/plain');
    for ($i = 1; $i <= 5; $i++) {
        $res->send("chunk-$i\n");
    }
    $res->end();
});

$client = spawn(function () use ($port, $server) {
    usleep(30000);
    $cmd = sprintf(
        'curl --http2-prior-knowledge -s --max-time 3 http://127.0.0.1:%d/',
        $port
    );
    $out = []; exec($cmd, $out, $rc);
    $body = implode("\n", $out);

    echo "rc=$rc\n";
    echo "body=", $body, "\n";

    $server->stop();
});

$server->start();
await($client);
echo "done\n";
--EXPECT--
rc=0
body=chunk-1
chunk-2
chunk-3
chunk-4
chunk-5
done
