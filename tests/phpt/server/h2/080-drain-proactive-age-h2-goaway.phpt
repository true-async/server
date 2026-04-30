--TEST--
HttpServer: hard-cap transition causes HTTP/2 GOAWAY on next commit
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
/* Step 8 — HTTP/2 reactive drain. With max_connections=1, the very
 * first accept trips the hard cap (pause_high = max_connections).
 * pause_listeners fires with drain_connections=true → epoch bumped.
 * The accepted H2 session picks up the new epoch on its first stream
 * commit and bundles GOAWAY(NO_ERROR) with the response. Client
 * receives its response; subsequent requests must open a new TCP. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

$port = 19842 + getmypid() % 100;

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(10)
    ->setWriteTimeout(10)
    ->setKeepAliveTimeout(30)
    ->setMaxConnections(1);   /* every accept is a drain trigger */

$server = new HttpServer($config);
$server->addHttpHandler(function ($req, $res) {
    $res->setStatusCode(200)->setBody('ok');
});

$client = spawn(function () use ($port, $server) {
    usleep(30000);

    /* Run curl against H2 prior-knowledge. Verbose output lets us
     * grep for the GOAWAY frame arrival. */
    $cmd = sprintf(
        'curl --http2-prior-knowledge -sv --max-time 3 '
        . 'http://127.0.0.1:%d/probe 2>&1',
        $port
    );
    $out = []; exec($cmd, $out, $rc);
    $blob = implode("\n", $out);

    echo "rc=$rc\n";
    /* curl verbose prints "GOAWAY" when it processes the frame. */
    echo "saw_goaway=", (int)(stripos($blob, 'GOAWAY') !== false), "\n";

    /* Wait for cleanup — let the response dispose complete and the
     * telemetry counter land. */
    usleep(100000);

    $tel = $server->getTelemetry();
    echo "goaway_total>=1=",   ($tel['h2_goaway_sent_total'] >= 1 ? 1 : 0), "\n";
    echo "drain_events>=1=",   ($tel['drain_events_reactive_total'] >= 1 ? 1 : 0), "\n";

    $server->stop();
});

$server->start();
await($client);
echo "done\n";
--EXPECT--
rc=0
saw_goaway=1
goaway_total>=1=1
drain_events>=1=1
done
