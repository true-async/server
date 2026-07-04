--TEST--
gRPC: client-streaming — N request messages read via a readMessage() loop
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
/* Client-streaming RPC: the client sends three 5-byte-prefixed messages in
 * one request body; the handler drains them with a readMessage() loop and
 * replies with a single summary message. Exercises the deframer across
 * multiple messages in the buffered body. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

require_once __DIR__ . '/../_free_port.inc';

function grpc_frame(string $m): string {
    return "\x00" . pack('N', strlen($m)) . $m;
}
function grpc_deframe_first(string $buf): string {
    $len = unpack('N', substr($buf, 1, 4))[1];
    return substr($buf, 5, $len);
}

$port = tas_free_port();
$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(5)
    ->setWriteTimeout(5);

$server = new HttpServer($config);
$server->addGrpcHandler(function($req, $resp) {
    $req->awaitBody();
    $msgs = [];
    while (($m = $req->readMessage()) !== null) {
        $msgs[] = $m;
    }
    $resp->writeMessage("got " . count($msgs) . ": " . implode('|', $msgs));
});

$client = spawn(function() use ($port, $server) {
    usleep(30000);

    $body = grpc_frame('a') . grpc_frame('bb') . grpc_frame('ccc');
    $bodyfile = tempnam(sys_get_temp_dir(), 'grpcreq');
    $outfile  = tempnam(sys_get_temp_dir(), 'grpcout');
    file_put_contents($bodyfile, $body);

    $cmd = sprintf(
        'curl --http2-prior-knowledge -s -v --max-time 3 -H %s -H %s '
        . '--data-binary @%s -o %s http://127.0.0.1:%d/svc/Collect 2>&1',
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

    echo "saw_grpc_status=", (int)(strpos($verbose, 'grpc-status: 0') !== false), "\n";
    echo "reply=", grpc_deframe_first($resp), "\n";

    $server->stop();
});

$server->start();
await($client);
echo "Done\n";
--EXPECT--
saw_grpc_status=1
reply=got 3: a|bb|ccc
Done
