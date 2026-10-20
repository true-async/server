--TEST--
gRPC over HTTP/2: a request body past setMaxBodySize ends with grpc-status 8, not a bare reset (#426)
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
/* A gRPC call whose message is past the body limit was refused with
 * RST_STREAM(ENHANCE_YOUR_CALM): the client read a transport error and no
 * grpc-status. The refusal now answers the call as gRPC does an over-size
 * message, RESOURCE_EXHAUSTED (8) in a trailers-only response, and then stops
 * the upload with RST_STREAM(NO_ERROR). */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

require __DIR__ . '/../h2/_h2_skipif.inc';
require_once __DIR__ . '/../_free_port.inc';

$port = tas_free_port();
$server = new HttpServer((new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setMaxBodySize(16 * 1024)
    ->setReadTimeout(5)
    ->setWriteTimeout(5));

/* awaitBody() first, so the body is buffered and the cumulative limit is
 * what refuses it; a handler already in readMessage() streams the body and
 * meets the live-bytes cap instead (grpc/023). */
$server->addGrpcHandler(function ($req, $resp) {
    $req->awaitBody();
    $req->readMessage();
    $resp->writeMessage('unexpected');
});

$client = spawn(function () use ($port, $server) {
    usleep(80000);

    $msg  = str_repeat('a', 64 * 1024);
    $body = tempnam(sys_get_temp_dir(), 'grpcbig');
    file_put_contents($body, "\x00" . pack('N', strlen($msg)) . $msg);

    $verbose = (string) shell_exec(sprintf(
        'curl --http2-prior-knowledge -s -v --max-time 5 -H %s -H %s --data-binary @%s -o %s '
        . 'http://127.0.0.1:%d/svc/Upload 2>&1',
        escapeshellarg('content-type: application/grpc'),
        escapeshellarg('te: trailers'),
        escapeshellarg($body),
        escapeshellarg(h2_dev_null()),
        $port));
    @unlink($body);

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
