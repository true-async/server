--TEST--
gRPC: unary error — handler sets grpc-status without any message (Trailers-Only)
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
/* A gRPC handler that returns an error status and streams NO message. The
 * server must still emit :status 200 + the grpc-status / grpc-message
 * trailers (an empty-body / Trailers-Only style reply), driven through the
 * streaming EOF path so the trailer goes out. */

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
    // NOT_FOUND, no response message.
    $resp->setTrailer('grpc-status', '5')
         ->setTrailer('grpc-message', 'nope');
});

$client = spawn(function() use ($port, $server) {
    usleep(30000);

    $frame = "\x00" . pack('N', 3) . 'req';
    $bodyfile = tempnam(sys_get_temp_dir(), 'grpcreq');
    $outfile  = tempnam(sys_get_temp_dir(), 'grpcout');
    file_put_contents($bodyfile, $frame);

    $cmd = sprintf(
        'curl --http2-prior-knowledge -s -v --max-time 3 -H %s -H %s '
        . '--data-binary @%s -o %s http://127.0.0.1:%d/svc/M 2>&1',
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

    echo "saw_status_200=",  (int)(strpos($verbose, 'HTTP/2 200') !== false), "\n";
    echo "saw_grpc_status=", (int)(strpos($verbose, 'grpc-status: 5') !== false), "\n";
    echo "saw_grpc_msg=",    (int)(strpos($verbose, 'grpc-message: nope') !== false), "\n";
    echo "empty_body=",      (int)($resp === ''), "\n";

    $server->stop();
});

$server->start();
await($client);
echo "Done\n";
--EXPECT--
saw_status_200=1
saw_grpc_status=1
saw_grpc_msg=1
empty_body=1
Done
