--TEST--
HttpServer: h2_streams_active / h2_streams_opened_total telemetry (PLAN_HTTP2 Step 10)
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
use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;
use function Async\delay;

$port = 19900 + getmypid() % 80;

$server = new HttpServer(
    (new HttpServerConfig())
        ->addListener('127.0.0.1', $port)
        ->setReadTimeout(10)
        ->setWriteTimeout(10)
);

$server->addHttpHandler(function ($req, $res) {
    $res->setStatusCode(200)->setBody('ok');
});

$client = spawn(function () use ($port, $server) {
    usleep(30000);

    $t0 = $server->getTelemetry();
    echo "initial active=", $t0['h2_streams_active'],
         " opened_total=", $t0['h2_streams_opened_total'], "\n";

    /* Three sequential h2 requests — each opens (and closes) a
     * stream. Counter bumps by 3 total; active stays at 0 once
     * each request finishes (stream_close fires per request). */
    for ($i = 0; $i < 3; $i++) {
        exec(sprintf('curl --http2-prior-knowledge -s -o /dev/null '
                     . '--max-time 2 http://127.0.0.1:%d/', $port));
    }
    delay(50);  /* Let the last stream_close fire. */

    $t1 = $server->getTelemetry();
    echo "after 3 reqs active=", $t1['h2_streams_active'],
         " opened_total=", $t1['h2_streams_opened_total'], "\n";

    $server->resetTelemetry();
    $t2 = $server->getTelemetry();
    echo "after reset opened_total=", $t2['h2_streams_opened_total'], "\n";

    $server->stop();
});

$server->start();
await($client);
echo "done\n";
?>
--EXPECT--
initial active=0 opened_total=0
after 3 reqs active=0 opened_total=3
after reset opened_total=0
done
