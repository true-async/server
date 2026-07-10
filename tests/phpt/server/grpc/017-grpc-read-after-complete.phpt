--TEST--
gRPC: readMessage after the whole upload already completed — no streaming-upgrade deadlock
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
/* Regression for the complete-before-upgrade deadlock: with a body in the
 * 64 KiB..1 MiB upgrade band, the whole upload can be received and finalized
 * (buffered) before the handler coroutine gets its first slot. The lazy
 * body_upgrade_to_stream then flipped body_streaming on an already-complete
 * request — empty queue, EOF forever lost — and readMessage() parked until
 * the client died. The delay() below forces that ordering deterministically:
 * the handler sleeps past the entire upload before its first readMessage(). */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;
use function Async\delay;

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
    /* Let the whole upload land + finalize before the first read. */
    delay(400);

    while (($m = $req->readMessage()) !== null) {
        $resp->writeMessage('len:' . strlen($m));
    }
});

$sizes = [40000, 45000, 50000];

$client = spawn(function () use ($port, $server, $sizes) {
    usleep(80000);

    $body = '';
    foreach ($sizes as $i => $sz) {
        $body .= grpc_frame(str_repeat(chr(ord('a') + $i), $sz));
    }
    $bodyfile = tempnam(sys_get_temp_dir(), 'grpccomplete');
    $outfile  = tempnam(sys_get_temp_dir(), 'grpcout');
    file_put_contents($bodyfile, $body);

    $cmd = sprintf(
        'curl --http2-prior-knowledge -s --max-time 8 -H %s -H %s '
        . '--data-binary @%s -o %s http://127.0.0.1:%d/svc/Collect 2>&1',
        escapeshellarg('content-type: application/grpc'),
        escapeshellarg('te: trailers'),
        escapeshellarg($bodyfile),
        escapeshellarg($outfile),
        $port
    );
    shell_exec($cmd);
    $resp = file_get_contents($outfile);
    @unlink($bodyfile);
    @unlink($outfile);

    $replies = grpc_deframe_all($resp);
    echo "count=", count($replies), "\n";
    echo "replies=", implode(',', $replies), "\n";

    $server->stop();
});

$server->start();
await($client);
echo "Done\n";
--EXPECT--
count=3
replies=len:40000,len:45000,len:50000
Done
