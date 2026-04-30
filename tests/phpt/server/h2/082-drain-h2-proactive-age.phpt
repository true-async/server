--TEST--
HttpServer: HTTP/2 proactive MAX_CONNECTION_AGE → GOAWAY on next commit
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
/* Step 8 — proactive H2 drain coverage (analogue of 078 for HTTP/2).
 * Handler sleeps past max_connection_age_ms so the age timestamp
 * crosses during its own processing; when it commits, should_drain_now
 * flags drain_pending via the proactive branch and the commit path
 * bundles GOAWAY with the response. Curl's -v output shows the
 * GOAWAY reception. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;
use function Async\delay;

$port = 19844 + getmypid() % 100;

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(10)
    ->setWriteTimeout(10)
    ->setKeepAliveTimeout(30)
    ->setMaxConnectionAgeMs(1000);    /* 1 second */

$server = new HttpServer($config);
$server->addHttpHandler(function ($req, $res) {
    /* Sleep past the 1 s age boundary so commit-time is post-expiry. */
    delay(1200);
    $res->setStatusCode(200)->setBody('aged');
});

$client = spawn(function () use ($port, $server) {
    usleep(30000);

    $cmd = sprintf(
        'curl --http2-prior-knowledge -sv --max-time 5 '
        . 'http://127.0.0.1:%d/slow 2>&1',
        $port
    );
    $out = []; exec($cmd, $out, $rc);
    $blob = implode("\n", $out);

    echo "rc=$rc\n";
    echo "has_body=",    (int)(strpos($blob, 'aged') !== false), "\n";
    echo "saw_goaway=",  (int)(stripos($blob, 'GOAWAY') !== false), "\n";

    /* Let the dispose unwind. */
    usleep(100000);

    $tel = $server->getTelemetry();
    echo "proactive_total=", (int)$tel['connections_drained_proactive_total'], "\n";
    echo "goaway_total=",    (int)$tel['h2_goaway_sent_total'], "\n";

    $server->stop();
});

$server->start();
await($client);
echo "done\n";
--EXPECT--
rc=0
has_body=1
saw_goaway=1
proactive_total=1
goaway_total=1
done
