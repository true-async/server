--TEST--
gRPC: server-streaming — one request, N framed response messages + trailer
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
/* Server-streaming RPC: the handler reads the single request message, then
 * emits N messages via writeMessage() (each a framed DATA slice), and the
 * server closes with grpc-status:0 in the terminal HEADERS(trailers). The
 * client reassembles every 5-byte-prefixed message from the response body. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

require_once __DIR__ . '/../_free_port.inc';

/* Deframe every gRPC message out of a raw body blob. */
function grpc_deframe(string $buf): array {
    $out = [];
    $off = 0;
    $n = strlen($buf);
    while ($off + 5 <= $n) {
        $len = unpack('N', substr($buf, $off + 1, 4))[1];
        if ($off + 5 + $len > $n) break;
        $out[] = substr($buf, $off + 5, $len);
        $off += 5 + $len;
    }
    return $out;
}

$port = tas_free_port();
$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(5)
    ->setWriteTimeout(5);

$server = new HttpServer($config);
$server->addGrpcHandler(function($req, $resp) {
    $req->awaitBody();
    $req->readMessage();               // consume the single request message
    for ($i = 1; $i <= 3; $i++) {
        $resp->writeMessage("resp-$i");
    }
    // grpc-status defaults to 0
});

$client = spawn(function() use ($port, $server) {
    usleep(30000);

    $frame = "\x00" . pack('N', 4) . 'ping';
    $bodyfile = tempnam(sys_get_temp_dir(), 'grpcreq');
    $outfile  = tempnam(sys_get_temp_dir(), 'grpcout');
    file_put_contents($bodyfile, $frame);

    $cmd = sprintf(
        'curl --http2-prior-knowledge -s -v --max-time 3 -H %s -H %s '
        . '--data-binary @%s -o %s http://127.0.0.1:%d/svc/Stream 2>&1',
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

    $msgs = grpc_deframe($resp);

    echo "saw_grpc_status=", (int)(strpos($verbose, 'grpc-status: 0') !== false), "\n";
    echo "count=", count($msgs), "\n";
    echo "msgs=", implode(',', $msgs), "\n";

    $server->stop();
});

$server->start();
await($client);
echo "Done\n";
--EXPECT--
saw_grpc_status=1
count=3
msgs=resp-1,resp-2,resp-3
Done
