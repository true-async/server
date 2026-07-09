--TEST--
gRPC: unary echo over h2c — framing + grpc-status trailer round-trip
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
/* End-to-end unary gRPC: the handler (addGrpcHandler) deframes the request
 * message via readMessage(), echoes it back framed via writeMessage(), and
 * lets the server default grpc-status:0. The client crafts a 5-byte-prefixed
 * request frame, POSTs it as application/grpc over h2c, and checks the
 * framed echo + the grpc-status trailer. */

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
$server->addGrpcHandler(function($req, $resp) {
    $req->awaitBody();
    $msg = $req->readMessage();
    $resp->writeMessage($msg === null ? '' : $msg);
    // grpc-status defaults to 0 (OK)
});

$client = spawn(function() use ($port, $server) {
    usleep(30000);

    $payload = 'hello-grpc';
    $frame   = "\x00" . pack('N', strlen($payload)) . $payload;   // 5-byte prefix
    $bodyfile = tempnam(sys_get_temp_dir(), 'grpcreq');
    $outfile  = tempnam(sys_get_temp_dir(), 'grpcout');
    file_put_contents($bodyfile, $frame);

    $cmd = sprintf(
        'curl --http2-prior-knowledge -s -v --max-time 3 '
        . '-H %s -H %s --data-binary @%s -o %s '
        . 'http://127.0.0.1:%d/helloworld.Greeter/SayHello 2>&1',
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

    $expected = "\x00" . pack('N', strlen($payload)) . $payload;

    echo "saw_status_200=",  (int)(strpos($verbose, 'HTTP/2 200') !== false), "\n";
    echo "saw_ctype=",       (int)(strpos($verbose, 'content-type: application/grpc') !== false), "\n";
    echo "saw_grpc_status=", (int)(strpos($verbose, 'grpc-status: 0') !== false), "\n";
    echo "echo_matches=",    (int)($resp === $expected), "\n";

    $server->stop();
});

$server->start();
await($client);
echo "Done\n";
--EXPECT--
saw_status_200=1
saw_ctype=1
saw_grpc_status=1
echo_matches=1
Done
