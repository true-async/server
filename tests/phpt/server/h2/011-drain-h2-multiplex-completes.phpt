--TEST--
HttpServer: GOAWAY bundled with final DATA — existing streams complete normally
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
/* Step 8 — GOAWAY must not strand in-flight work. With
 * max_connections=1 every accept trips the cap + drains, yet the
 * client's current request has to round-trip cleanly (handler
 * finishes → full response + GOAWAY bundled → client reads both). */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;
use function Async\delay;

$port = 19843 + getmypid() % 100;

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(10)
    ->setWriteTimeout(10)
    ->setKeepAliveTimeout(30)
    ->setMaxConnections(1);

$server = new HttpServer($config);
$server->addHttpHandler(function ($req, $res) {
    /* Deliberately slow — simulates mid-handler-work when drain
     * fires (it already fired at accept, but handler is going
     * to commit later). Handler must complete cleanly regardless. */
    delay(100);
    $res->setStatusCode(200)
        ->setHeader('X-Marker', 'completed')
        ->setBody('payload-42');
});

$client = spawn(function () use ($port, $server) {
    usleep(30000);

    /* -i puts headers into stdout so we can assert both body AND
     * header survived the drain-bundling. */
    $cmd = sprintf(
        'curl --http2-prior-knowledge -si --max-time 3 '
        . 'http://127.0.0.1:%d/slow',
        $port
    );
    $out = []; exec($cmd, $out, $rc);
    $blob = implode("\n", $out);

    echo "rc=$rc\n";
    echo "has_body=",   (int)(strpos($blob, 'payload-42') !== false), "\n";
    echo "has_marker=", (int)(stripos($blob, 'x-marker: completed') !== false), "\n";

    /* Let the connection finish draining before reading telemetry. */
    usleep(150000);

    $tel = $server->getTelemetry();
    echo "goaway_total>=1=", ($tel['h2_goaway_sent_total'] >= 1 ? 1 : 0), "\n";

    $server->stop();
});

$server->start();
await($client);
echo "done\n";
--EXPECT--
rc=0
has_body=1
has_marker=1
goaway_total>=1=1
done
