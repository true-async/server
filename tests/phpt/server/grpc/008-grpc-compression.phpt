--TEST--
gRPC: per-message gzip — compressed request in, compressed response out
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
require __DIR__ . '/../h2/_h2_skipif.inc';
h2_skipif(['curl_h2' => true]);
if (!function_exists('gzencode')) die('skip zlib ext not available');
?>
--FILE--
<?php
/* Per-message compression both directions: the client sends a gzip-compressed
 * message (flag=1, grpc-encoding: gzip); readMessage() transparently inflates
 * it. The handler replies with writeMessage(..., compress: true), so the
 * response message carries the compressed flag + grpc-encoding: gzip and the
 * client gzdecodes it. Payload is 10 KB so the inflate grow-loop is used. */

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
    $msg   = $req->readMessage();               // auto-inflated
    $empty = $req->readMessage();               // compressed empty message -> ""
    $tail  = ($empty === '') ? ':empty-ok' : ':empty-bad';
    $resp->writeMessage('echo:' . $msg . $tail, true);  // gzip the reply
});

$client = spawn(function() use ($port, $server) {
    usleep(30000);

    $payload = str_repeat('gRPC-', 2000);        // 10 KB
    $gz      = gzencode($payload);
    $frame   = "\x01" . pack('N', strlen($gz)) . $gz;   // compressed flag = 1
    $frame  .= "\x01" . pack('N', 0);                   // compressed EMPTY message

    $bodyfile = tempnam(sys_get_temp_dir(), 'grpcreq');
    $outfile  = tempnam(sys_get_temp_dir(), 'grpcout');
    file_put_contents($bodyfile, $frame);

    $cmd = sprintf(
        'curl --http2-prior-knowledge -s -v --max-time 3 -H %s -H %s -H %s '
        . '--data-binary @%s -o %s http://127.0.0.1:%d/svc/M 2>&1',
        escapeshellarg('content-type: application/grpc'),
        escapeshellarg('grpc-encoding: gzip'),
        escapeshellarg('te: trailers'),
        escapeshellarg($bodyfile),
        escapeshellarg($outfile),
        $port
    );
    $verbose = shell_exec($cmd);
    $resp    = file_get_contents($outfile);
    @unlink($bodyfile);
    @unlink($outfile);

    $flag    = strlen($resp) ? ord($resp[0]) : -1;
    $len     = strlen($resp) >= 5 ? unpack('N', substr($resp, 1, 4))[1] : 0;
    $decoded = ($flag === 1 && $len > 0) ? @gzdecode(substr($resp, 5, $len)) : '';

    echo "resp_grpc_encoding=", (int)(strpos($verbose, 'grpc-encoding: gzip') !== false), "\n";
    echo "resp_compressed_flag=", (int)($flag === 1), "\n";
    echo "roundtrip_ok=", (int)($decoded === 'echo:' . $payload . ':empty-ok'), "\n";
    echo "saw_grpc_status=", (int)(strpos($verbose, 'grpc-status: 0') !== false), "\n";

    $server->stop();
});

$server->start();
await($client);
echo "Done\n";
--EXPECT--
resp_grpc_encoding=1
resp_compressed_flag=1
roundtrip_ok=1
saw_grpc_status=1
Done
