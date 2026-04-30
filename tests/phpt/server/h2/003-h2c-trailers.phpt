--TEST--
HttpServer: HTTP/2 response trailers (gRPC-style)
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
/* End-to-end trailer delivery — the unlock for gRPC unary RPCs.
 * Handler sets a normal response + two trailers; server serialises
 * HEADERS(:status) → DATA(NO_END_STREAM) → HEADERS(trailers,
 * END_STREAM). curl -v surfaces the trailer lines after a blank
 * line separating them from the initial headers. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

$port = 19720 + getmypid() % 100;

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(5)
    ->setWriteTimeout(5);

$server = new HttpServer($config);
$server->addHttpHandler(function($req, $resp) {
    $resp->setStatusCode(200)
         ->setHeader('Content-Type', 'application/grpc')
         ->setTrailer('grpc-status', '0')
         ->setTrailer('grpc-message', 'ok')
         ->setBody('payload');
});

$client = spawn(function() use ($port, $server) {
    usleep(30000);
    $cmd = sprintf(
        'curl --http2-prior-knowledge -s -v --max-time 3 http://127.0.0.1:%d/ 2>&1',
        $port
    );
    $out = [];
    exec($cmd, $out, $rc);
    $blob = implode("\n", $out);

    echo "curl_rc=$rc\n";
    echo "saw_status_200=",   (int)(strpos($blob, 'HTTP/2 200') !== false), "\n";
    echo "saw_ctype=",        (int)(strpos($blob, 'application/grpc') !== false), "\n";
    /* Trailers appear after the body: curl prints them as separate
     * response-header lines because they arrive in a HEADERS frame. */
    echo "saw_grpc_status=",  (int)(strpos($blob, 'grpc-status: 0') !== false), "\n";
    echo "saw_grpc_message=", (int)(strpos($blob, 'grpc-message: ok') !== false), "\n";

    $server->stop();
});

$server->start();
await($client);
echo "Done\n";
--EXPECT--
curl_rc=0
saw_status_200=1
saw_ctype=1
saw_grpc_status=1
saw_grpc_message=1
Done
