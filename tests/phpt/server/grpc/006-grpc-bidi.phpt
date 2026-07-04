--TEST--
gRPC: bidi (half-duplex) — read N request messages, write N responses
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
/* Bidirectional-streaming RPC on one stream: the handler reads every request
 * message and writes a response per message. This is the half-duplex shape
 * (readMessage() drains the buffered request before the responses stream) —
 * true full-duplex interleaving needs the incremental body path (follow-up).
 * It still exercises N-in / N-out framing on a single multiplexed stream. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

require_once __DIR__ . '/../_free_port.inc';

function grpc_frame(string $m): string {
    return "\x00" . pack('N', strlen($m)) . $m;
}
function grpc_deframe(string $buf): array {
    $out = []; $off = 0; $n = strlen($buf);
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
    while (($m = $req->readMessage()) !== null) {
        $resp->writeMessage("echo:$m");
    }
});

$client = spawn(function() use ($port, $server) {
    usleep(30000);

    $body = grpc_frame('x') . grpc_frame('y');
    $bodyfile = tempnam(sys_get_temp_dir(), 'grpcreq');
    $outfile  = tempnam(sys_get_temp_dir(), 'grpcout');
    file_put_contents($bodyfile, $body);

    $cmd = sprintf(
        'curl --http2-prior-knowledge -s -v --max-time 3 -H %s -H %s '
        . '--data-binary @%s -o %s http://127.0.0.1:%d/svc/Chat 2>&1',
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
count=2
msgs=echo:x,echo:y
Done
