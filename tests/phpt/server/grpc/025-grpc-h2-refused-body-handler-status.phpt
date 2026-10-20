--TEST--
gRPC over HTTP/2: a handler that catches a refused body keeps the grpc-status it sets (#426)
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
/* The test sends the first byte of the message with the request and the rest
 * 0.3 s later, so the handler is parked in awaitBody() when the body is refused.
 * It catches the 413 and answers with a grpc-status of its own: the refusal
 * fills in RESOURCE_EXHAUSTED only when the handler set none, and adds its
 * grpc-message only then, so the handler's status keeps its own message. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\HttpException;
use function Async\spawn;
use function Async\await;

require __DIR__ . '/../h2/_h2_skipif.inc';
require __DIR__ . '/_grpc_h2_upload.inc';
require_once __DIR__ . '/../_free_port.inc';

$port = tas_free_port();
$server = new HttpServer((new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setMaxBodySize(16 * 1024)
    ->setReadTimeout(5)
    ->setWriteTimeout(5));

$server->addGrpcHandler(function ($req, $resp) {
    try {
        $req->awaitBody();
    } catch (HttpException $e) {
        $resp->setTrailer('grpc-status', '9')
             ->setTrailer('grpc-message', 'handler says no');
    }
});

$client = spawn(function () use ($port, $server) {
    usleep(80000);

    $msg = str_repeat('a', 64 * 1024);

    $verbose = grpc_h2_upload_timed($port, "\x00" . pack('N', strlen($msg)) . $msg);

    echo 'http 200: ', (int) (preg_match('/< HTTP\/2 200/', $verbose) === 1), "\n";
    echo 'grpc-status: ', preg_match('/< grpc-status: (\d+)/', $verbose, $m) ? $m[1] : 'none', "\n";
    echo 'grpc-message: ', preg_match('/< grpc-message: ([^\r\n]*)/', $verbose, $m) ? $m[1] : 'none', "\n";
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
grpc-status: 9
grpc-message: handler says no
error reset: 0
done
