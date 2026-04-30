--TEST--
HttpServer: streaming telemetry counters advance on send() / reset() clears
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
/* PLAN_STREAMING §7 telemetry: verify that send()-based responses
 * bump streaming_responses_total / stream_send_calls_total /
 * stream_bytes_sent_total, while a buffered-mode response leaves
 * them untouched. resetTelemetry() zeros them. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

$port = 19910 + getmypid() % 80;

$server = new HttpServer(
    (new HttpServerConfig())
        ->addListener('127.0.0.1', $port)
        ->setReadTimeout(10)
        ->setWriteTimeout(10)
);

$server->addHttpHandler(function ($req, $res) {
    $path = $req->getUri();
    if ($path === '/stream') {
        $res->setStatusCode(200)
            ->setHeader('Content-Type', 'text/plain');
        $res->send("aaa");   // 3 bytes
        $res->send("bbbbb"); // 5 bytes
        $res->end();
    } else {
        $res->setStatusCode(200)->setBody("buffered\n")->end();
    }
});

$client = spawn(function () use ($port, $server) {
    usleep(30000);
    /* Buffered request first — telemetry should stay at zero for streaming counters. */
    exec(sprintf('curl --http2-prior-knowledge -s --max-time 3 http://127.0.0.1:%d/plain -o /dev/null', $port));

    $t0 = $server->getTelemetry();
    echo "after-buffered streaming_responses=", $t0['streaming_responses_total'], "\n";
    echo "after-buffered stream_send_calls=",    $t0['stream_send_calls_total'], "\n";
    echo "after-buffered stream_bytes_sent=",    $t0['stream_bytes_sent_total'], "\n";

    /* Two streaming requests, each with 2 send() calls, 8 bytes total. */
    exec(sprintf('curl --http2-prior-knowledge -s --max-time 3 http://127.0.0.1:%d/stream -o /dev/null', $port));
    exec(sprintf('curl --http2-prior-knowledge -s --max-time 3 http://127.0.0.1:%d/stream -o /dev/null', $port));

    $t1 = $server->getTelemetry();
    echo "after-stream  streaming_responses=", $t1['streaming_responses_total'], "\n";
    echo "after-stream  stream_send_calls=",    $t1['stream_send_calls_total'], "\n";
    echo "after-stream  stream_bytes_sent=",    $t1['stream_bytes_sent_total'], "\n";

    $server->resetTelemetry();
    $t2 = $server->getTelemetry();
    echo "after-reset   streaming_responses=", $t2['streaming_responses_total'], "\n";
    echo "after-reset   stream_send_calls=",    $t2['stream_send_calls_total'], "\n";
    echo "after-reset   stream_bytes_sent=",    $t2['stream_bytes_sent_total'], "\n";

    $server->stop();
});

$server->start();
await($client);
echo "done\n";
?>
--EXPECT--
after-buffered streaming_responses=0
after-buffered stream_send_calls=0
after-buffered stream_bytes_sent=0
after-stream  streaming_responses=2
after-stream  stream_send_calls=4
after-stream  stream_bytes_sent=16
after-reset   streaming_responses=0
after-reset   stream_send_calls=0
after-reset   stream_bytes_sent=0
done
