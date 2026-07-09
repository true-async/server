--TEST--
gRPC-Web: trailers carried in-body as a 0x80 frame (binary)
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
/* grpc-web (binary, application/grpc-web+proto): same 5-byte message framing
 * as native gRPC, but the grpc-status/grpc-message trailers ride the response
 * body as a 0x80-flagged frame — browsers can't read HTTP/2 trailers. The
 * same addGrpcHandler / readMessage / writeMessage API is used; only the
 * finalize differs. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

require_once __DIR__ . '/../_free_port.inc';

/* Split a grpc-web body into data messages + the trailer block. */
function grpcweb_parse(string $buf): array {
    $data = []; $trailers = ''; $off = 0; $n = strlen($buf);
    while ($off + 5 <= $n) {
        $flag = ord($buf[$off]);
        $len  = unpack('N', substr($buf, $off + 1, 4))[1];
        if ($off + 5 + $len > $n) break;
        $payload = substr($buf, $off + 5, $len);
        if ($flag & 0x80) { $trailers .= $payload; }
        else              { $data[] = $payload; }
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
    $msg = $req->readMessage();
    $resp->writeMessage('echo:' . $msg);
    // grpc-status defaults to 0
});

$client = spawn(function() use ($port, $server) {
    usleep(30000);

    $frame = "\x00" . pack('N', 4) . 'ping';
    $bodyfile = tempnam(sys_get_temp_dir(), 'grpcreq');
    $outfile  = tempnam(sys_get_temp_dir(), 'grpcout');
    file_put_contents($bodyfile, $frame);

    $cmd = sprintf(
        'curl --http2-prior-knowledge -s -v --max-time 3 -H %s '
        . '--data-binary @%s -o %s http://127.0.0.1:%d/svc/M 2>&1',
        escapeshellarg('content-type: application/grpc-web+proto'),
        escapeshellarg($bodyfile),
        escapeshellarg($outfile),
        $port
    );
    $verbose = shell_exec($cmd);
    $resp    = file_get_contents($outfile);
    @unlink($bodyfile);
    @unlink($outfile);

    [$data, $trailers] = grpcweb_parse($resp);

    echo "resp_ctype_web=",  (int)(strpos($verbose, 'content-type: application/grpc-web+proto') !== false), "\n";
    echo "data=",            implode(',', $data), "\n";
    echo "trailer_status=",  (int)(strpos($trailers, 'grpc-status: 0') !== false), "\n";
    /* The status must be in-body, not an HTTP trailer line. */
    echo "no_http_trailer=", (int)(strpos($verbose, "\ngrpc-status:") === false && strpos($verbose, "< grpc-status") === false), "\n";

    $server->stop();
});

$server->start();
await($client);
echo "Done\n";
--EXPECT--
resp_ctype_web=1
data=echo:ping
trailer_status=1
no_http_trailer=1
Done
