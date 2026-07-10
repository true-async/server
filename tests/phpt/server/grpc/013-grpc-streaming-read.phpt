--TEST--
gRPC: incremental readMessage over a streaming request body (true full-duplex)
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
/* With setBodyStreamingEnabled(true) the handler drains request messages via
 * readMessage() WITHOUT awaitBody() — reading each message as it arrives while
 * the client is still sending (client-streaming / true full-duplex). Each
 * message here is ~40 KiB, so it spans several 16 KiB DATA frames: this pins
 * the incremental reassembler (deframe across frame boundaries). Total body is
 * in the 64 KiB..1 MiB upgrade band, so the parser switches to the streaming
 * queue. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

require_once __DIR__ . '/../_free_port.inc';

function grpc_frame(string $m): string {
    return "\x00" . pack('N', strlen($m)) . $m;
}
function grpc_deframe_all(string $buf): array {
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
    ->setBodyStreamingEnabled(true)
    ->setMaxBodySize(8 * 1024 * 1024)
    ->setReadTimeout(10)
    ->setWriteTimeout(10);

$server = new HttpServer($config);
$server->addGrpcHandler(function ($req, $resp) {
    /* No awaitBody() — drain messages incrementally as they arrive. */
    while (($m = $req->readMessage()) !== null) {
        $resp->writeMessage('len:' . strlen($m));
    }
    // grpc-status defaults to 0
});

$sizes = [40000, 45000, 50000];

$client = spawn(function () use ($port, $server, $sizes) {
    usleep(80000);

    $body = '';
    foreach ($sizes as $i => $sz) {
        $body .= grpc_frame(str_repeat(chr(ord('a') + $i), $sz));
    }
    $bodyfile = tempnam(sys_get_temp_dir(), 'grpcstream');
    $outfile  = tempnam(sys_get_temp_dir(), 'grpcout');
    file_put_contents($bodyfile, $body);

    $cmd = sprintf(
        'curl --http2-prior-knowledge -s -v --max-time 5 -H %s -H %s '
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

    $replies = grpc_deframe_all($resp);

    echo "saw_grpc_status=", (int)(strpos($verbose, 'grpc-status: 0') !== false), "\n";
    echo "count=", count($replies), "\n";
    echo "replies=", implode(',', $replies), "\n";

    if (count($replies) === 0) {
        /* Flake forensics (~1/200: empty response within max-time): dump the
         * curl verbose into the failing output so the next occurrence is
         * diagnosable straight from the CI diff. */
        echo "--- curl verbose ---\n", $verbose, "\n";
    }

    $server->stop();
});

$server->start();
await($client);
echo "Done\n";
--EXPECT--
saw_grpc_status=1
count=3
replies=len:40000,len:45000,len:50000
Done
