--TEST--
gRPC over HTTP/2: a handler that catches the refusal and reads again gets 413 again, and the call ends with grpc-status 8 (#426)
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
/* The test sends the first byte of the message with the request and the rest 0.3 s
 * later, so the handler is parked in awaitBody() by then and is there to catch.
 * The refusal reaches it as an HttpException (413). A handler that catches it
 * and reads the message anyway must get the same exception, not park on a body
 * that will not come and not read null as a client half-close. The call still
 * ends with RESOURCE_EXHAUSTED (8), the handler having set no status of its own. */

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

$seen = [];

$server->addGrpcHandler(function ($req, $resp) use (&$seen) {
    try {
        $req->awaitBody();
        $seen[] = "first: no refusal";
    } catch (HttpException $e) {
        $seen[] = "first: " . $e->getCode();
    }

    try {
        $req->readMessage();
        $seen[] = "second: no refusal";
    } catch (HttpException $e) {
        $seen[] = "second: " . $e->getCode();
    }
});

$client = spawn(function () use ($port, $server, &$seen) {
    usleep(80000);

    $msg = str_repeat('a', 64 * 1024);

    $verbose = grpc_h2_upload_timed($port, "\x00" . pack('N', strlen($msg)) . $msg);

    echo 'http 200: ', (int) (preg_match('/< HTTP\/2 200/', $verbose) === 1), "\n";
    echo 'grpc-status: ', preg_match('/< grpc-status: (\d+)/', $verbose, $m) ? $m[1] : 'none', "\n";
    echo 'grpc-message: ', (int) (stripos($verbose, '< grpc-message:') !== false), "\n";
    echo 'error reset: ', (int) (stripos($verbose, 'not closed cleanly') !== false
        && stripos($verbose, 'NO_ERROR') === false), "\n";

    echo implode("\n", $seen), "\n";

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
first: 413
second: 413
done
