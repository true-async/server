--TEST--
gRPC: a handler killed by a bailout reports INTERNAL, not OK
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
require __DIR__ . '/../h2/_h2_skipif.inc';
h2_skipif(['curl_h2' => true]);
?>
--INI--
memory_limit=8M
display_errors=0
log_errors=0
--FILE--
<?php
/* A gRPC call reports its outcome in the grpc-status trailer, and the dispose
 * derived that outcome from `coroutine->exception`. A zend_bailout leaves the
 * exception NULL, so a handler killed mid-call was reported as grpc-status 0 —
 * a success over a message it never finished writing. It has to be 13
 * (INTERNAL), the same status a thrown exception gets. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

require_once __DIR__ . '/../_free_port.inc';

$port = tas_free_port();

$server = new HttpServer(
    (new HttpServerConfig())
        ->addListener('127.0.0.1', $port)
        ->setReadTimeout(5)
        ->setWriteTimeout(5)
);

$server->addGrpcHandler(function ($req, $resp) {
    $req->awaitBody();
    $doomed = str_repeat('a', 64 * 1024 * 1024);
    $resp->setTrailer('grpc-status', '0');
});

$client = spawn(function () use ($port, $server) {
    usleep(30000);

    $bodyfile = tempnam(sys_get_temp_dir(), 'grpcreq');
    $outfile  = tempnam(sys_get_temp_dir(), 'grpcout');
    file_put_contents($bodyfile, "\x00" . pack('N', 3) . 'req');

    $verbose = (string)shell_exec(sprintf(
        'curl --http2-prior-knowledge -s -v --max-time 5 -H %s -H %s '
        . '--data-binary @%s -o %s http://127.0.0.1:%d/svc/M 2>&1',
        escapeshellarg('content-type: application/grpc'),
        escapeshellarg('te: trailers'),
        escapeshellarg($bodyfile), escapeshellarg($outfile), $port));

    @unlink($bodyfile);
    @unlink($outfile);
    $server->stop();

    /* Printed after the request: the firewall dumps a C backtrace to stderr,
     * and run-tests compares stderr along with stdout. */
    echo 'saw_status_200=',  (int)(strpos($verbose, 'HTTP/2 200') !== false), "\n";
    echo 'saw_grpc_status=', (int)(strpos($verbose, 'grpc-status: 13') !== false), "\n";
});

$server->start();
await($client);
echo "Done\n";
--EXPECTF--
%Asaw_status_200=1
saw_grpc_status=1
Done
