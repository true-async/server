--TEST--
gRPC-Web: zero-message error — status/message in the in-body trailer frame
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
/* grpc-web handler that returns an error with no message. The reply is a
 * single DATA frame — the 0x80 trailer frame carrying grpc-status/grpc-message
 * — committed straight from the append (no messages were streamed). */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

require_once __DIR__ . '/../_free_port.inc';

function grpcweb_parse(string $buf): array {
    $data = []; $trailers = ''; $off = 0; $n = strlen($buf);
    while ($off + 5 <= $n) {
        $flag = ord($buf[$off]);
        $len  = unpack('N', substr($buf, $off + 1, 4))[1];
        if ($off + 5 + $len > $n) break;
        $payload = substr($buf, $off + 5, $len);
        if ($flag & 0x80) { $trailers .= $payload; } else { $data[] = $payload; }
        $off += 5 + $len;
    }
    return [$data, $trailers];
}

$port = tas_free_port();
$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(5)
    ->setWriteTimeout(5);

$server = new HttpServer($config);
$server->addGrpcHandler(function($req, $resp) {
    $req->awaitBody();
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
        'curl --http2-prior-knowledge -s -v --max-time 3 -H %s '
        . '--data-binary @%s -o %s http://127.0.0.1:%d/svc/M 2>&1',
        escapeshellarg('content-type: application/grpc-web'),
        escapeshellarg($bodyfile),
        escapeshellarg($outfile),
        $port
    );
    $verbose = shell_exec($cmd);
    $resp    = file_get_contents($outfile);
    @unlink($bodyfile);
    @unlink($outfile);

    [$data, $trailers] = grpcweb_parse($resp);

    echo "data_count=",     count($data), "\n";
    echo "trailer_status=", (int)(strpos($trailers, 'grpc-status: 5') !== false), "\n";
    echo "trailer_msg=",    (int)(strpos($trailers, 'grpc-message: nope') !== false), "\n";

    $server->stop();
});

$server->start();
await($client);
echo "Done\n";
--EXPECT--
data_count=0
trailer_status=1
trailer_msg=1
Done
