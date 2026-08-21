--TEST--
gRPC: a handler that throws after writing a message still ends with grpc-status
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
/* gRPC has its own way of reporting a failed call, and it is not the one HTTP
 * uses: the status rides in trailers after the messages, so a call that fails
 * halfway still ends the stream cleanly and says INTERNAL. Resetting it
 * instead would take the status away and leave the client with a transport
 * error where the protocol has a perfectly good answer.
 *
 * That is why the abort routing is gated on the call not being gRPC, and this
 * is the test that keeps the gate: a handler that writes one message and then
 * throws must produce that message, `grpc-status: 13`, and a stream the client
 * saw end. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

require_once __DIR__ . '/../_free_port.inc';

$port = tas_free_port();
$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(5)
    ->setWriteTimeout(5);

$server = new HttpServer($config);
$server->addGrpcHandler(function ($req, $resp) {
    $req->awaitBody();
    $req->readMessage();
    $resp->writeMessage('partial');

    throw new \RuntimeException('handler failed after one message');
});

$client = spawn(function () use ($port, $server) {
    usleep(30000);

    $frame    = "\x00" . pack('N', 4) . 'ping';
    $bodyfile = tempnam(sys_get_temp_dir(), 'grpcreq');
    $outfile  = tempnam(sys_get_temp_dir(), 'grpcout');
    file_put_contents($bodyfile, $frame);

    $cmd = sprintf(
        'curl --http2-prior-knowledge -s -v --max-time 3 -H %s -H %s '
        . '--data-binary @%s -o %s http://127.0.0.1:%d/svc/Fail 2>&1',
        escapeshellarg('content-type: application/grpc'),
        escapeshellarg('te: trailers'),
        escapeshellarg($bodyfile),
        escapeshellarg($outfile),
        $port
    );
    $verbose = shell_exec($cmd);
    $resp    = file_get_contents($outfile);
    @unlink($bodyfile);
    @unlink($outfile);

    echo "saw_status_200=",  (int) (strpos($verbose, 'HTTP/2 200') !== false), "\n";
    echo "saw_grpc_status=", (int) (strpos($verbose, 'grpc-status: 13') !== false), "\n";
    echo "message=", substr($resp, 5), "\n";

    $server->stop();
});

$server->start();
await($client);
echo "Done\n";
?>
--EXPECT--
saw_status_200=1
saw_grpc_status=1
message=partial
Done
