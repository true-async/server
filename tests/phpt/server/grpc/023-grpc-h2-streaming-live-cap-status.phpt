--TEST--
gRPC over HTTP/2: a streamed message past the live-bytes cap ends with grpc-status 8 (#426)
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
require __DIR__ . '/../h2/_h2_skipif.inc';
h2_skipif(['curl_h2' => true]);
?>
--FILE--
<?php
/* A handler already parked in readMessage() has its body streamed, so the cap
 * that refuses it is the live-bytes one: the bytes queued and not yet read.
 * the test sends the first byte of the message with the request and the rest 0.3 s
 * later, so the handler is parked by then, and the first DATA frame of the rest,
 * 16 KiB, is past the cap on its own. The refusal answers the call with
 * RESOURCE_EXHAUSTED (8) in a trailers-only response, as it does for a buffered
 * body (grpc/020), and stops the upload with RST_STREAM(NO_ERROR). */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

require __DIR__ . '/../h2/_h2_skipif.inc';
require __DIR__ . '/_grpc_h2_upload.inc';
require_once __DIR__ . '/../_free_port.inc';

$port = tas_free_port();
$server = new HttpServer((new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setMaxBodySize(8 * 1024)
    ->setBodyStreamingEnabled(true)
    ->setReadTimeout(5)
    ->setWriteTimeout(5));

$server->addGrpcHandler(function ($req, $resp) {
    $req->readMessage();
    $resp->writeMessage('unexpected');
});

$client = spawn(function () use ($port, $server) {
    usleep(80000);

    $msg  = str_repeat('a', 64 * 1024);
    $msg = str_repeat('a', 64 * 1024);

    $verbose = grpc_h2_upload_timed($port, "\x00" . pack('N', strlen($msg)) . $msg);

    echo 'http 200: ', (int) (preg_match('/< HTTP\/2 200/', $verbose) === 1), "\n";
    echo 'grpc-status: ', preg_match('/< grpc-status: (\d+)/', $verbose, $m) ? $m[1] : 'none', "\n";
    echo 'grpc-message: ', (int) (stripos($verbose, '< grpc-message:') !== false), "\n";
    echo 'error reset: ', (int) (stripos($verbose, 'not closed cleanly') !== false
        && stripos($verbose, 'NO_ERROR') === false), "\n";

    $server->stop();
});

$server->start();
await($client);
echo "done\n";
?>
--EXPECT--
http 200: 1
grpc-status: 8
grpc-message: 1
error reset: 0
done
