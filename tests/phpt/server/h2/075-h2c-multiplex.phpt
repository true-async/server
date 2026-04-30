--TEST--
HttpServer: HTTP/2 multiplex — multiple streams on one TCP connection
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
/* Step 7: N concurrent streams on one HTTP/2 TCP connection each
 * spawn their own handler coroutine with per-stream request_zv /
 * response_zv. This test proves the core multiplex invariant: 3
 * interleaved requests round-trip cleanly without trampling on
 * each other's state.
 *
 * `curl --parallel --parallel-immediate` over prior-knowledge h2
 * opens ONE TCP and multiplexes all 3 requests as separate streams
 * inside it. Handler stamps a per-request counter + echoes the URI;
 * we assert all 3 URIs appeared and the counter ran 1..3. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

$port = 19730 + getmypid() % 60;

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(5)
    ->setWriteTimeout(5);

$server = new HttpServer($config);
$counter = 0;
$server->addHttpHandler(function ($req, $resp) use (&$counter) {
    $n = ++$counter;
    $resp->setStatusCode(200)
         ->setHeader('Content-Type', 'text/plain')
         ->setBody(sprintf("n=%d uri=%s\n", $n, $req->getUri()));
});

$client = spawn(function () use ($port, $server) {
    usleep(30000);
    /* --parallel with HTTP/2 prior-knowledge → curl reuses the same
     * TCP for all 3 URLs as concurrent streams. */
    $cmd = sprintf(
        'curl --http2-prior-knowledge -s --parallel --parallel-immediate '
        . '--max-time 3 '
        . 'http://127.0.0.1:%d/a http://127.0.0.1:%d/b http://127.0.0.1:%d/c',
        $port, $port, $port
    );
    $out = [];
    exec($cmd, $out, $rc);
    $blob = implode("\n", $out);

    echo "rc=$rc\n";
    /* Any interleave is acceptable (multiplex); we only assert
     * each URI came back at least once. */
    echo "saw_a=", (int)(strpos($blob, 'uri=/a') !== false), "\n";
    echo "saw_b=", (int)(strpos($blob, 'uri=/b') !== false), "\n";
    echo "saw_c=", (int)(strpos($blob, 'uri=/c') !== false), "\n";
    /* Handler ran 3 times total → counter hit n=3 on the last one. */
    echo "saw_3_requests=", (int)(preg_match_all('/n=[123]\b/', $blob) === 3), "\n";

    $server->stop();
});

$server->start();
await($client);
echo "Done\n";
--EXPECT--
rc=0
saw_a=1
saw_b=1
saw_c=1
saw_3_requests=1
Done
