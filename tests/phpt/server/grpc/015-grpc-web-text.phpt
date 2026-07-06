--TEST--
gRPC-Web-Text: base64 framing both directions (per-frame encoding)
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
/* grpc-web-text (application/grpc-web-text): the grpc-web frame stream,
 * base64-encoded — the request body arrives base64, readMessage()
 * transparently decodes it; every response frame (each writeMessage and
 * the 0x80 trailer frame) goes out independently base64-encoded with its
 * own padding, as the grpc-web protocol allows. Two writeMessage calls
 * prove the per-frame (not whole-body) encoding: the concatenation of
 * two padded base64 blocks only parses if decoded per block. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

require_once __DIR__ . '/../_free_port.inc';

/* Decode a concatenation of independently-padded base64 frames, then split
 * into data messages + the trailer block (same as grpcweb_parse in 009). */
function grpcwebtext_parse(string $b64): array {
    $buf = '';
    /* Each frame's base64 block ends at a padding boundary; PHP's decoder
     * with $strict=false skips nothing here — split on '=' padding runs. */
    foreach (preg_split('/(?<==)(?=[A-Za-z0-9+\/])/', $b64) as $block) {
        $buf .= base64_decode($block);
    }
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
    $msg = $req->readMessage();          // base64-decoded transparently
    $resp->writeMessage('echo:' . $msg); // frame 1, base64 out
    $resp->writeMessage('bye');          // frame 2 — proves per-frame b64
    // grpc-status defaults to 0
});

$client = spawn(function() use ($port, $server) {
    usleep(30000);

    $frame = "\x00" . pack('N', 4) . 'ping';
    $bodyfile = tempnam(sys_get_temp_dir(), 'grpcreq');
    $outfile  = tempnam(sys_get_temp_dir(), 'grpcout');
    file_put_contents($bodyfile, base64_encode($frame));

    $cmd = sprintf(
        'curl --http2-prior-knowledge -s -v --max-time 3 -H %s '
        . '--data-binary @%s -o %s http://127.0.0.1:%d/svc/M 2>&1',
        escapeshellarg('content-type: application/grpc-web-text'),
        escapeshellarg($bodyfile),
        escapeshellarg($outfile),
        $port
    );
    $verbose = shell_exec($cmd);
    $resp    = file_get_contents($outfile);
    @unlink($bodyfile);
    @unlink($outfile);

    [$data, $trailers] = grpcwebtext_parse($resp);

    echo "resp_ctype_webtext=", (int)(strpos($verbose, 'content-type: application/grpc-web-text+proto') !== false), "\n";
    echo "body_is_base64=",     (int)(preg_match('/^[A-Za-z0-9+\/=]+$/', $resp) === 1), "\n";
    echo "data=",               implode(',', $data), "\n";
    echo "trailer_status=",     (int)(strpos($trailers, 'grpc-status: 0') !== false), "\n";
    echo "no_http_trailer=",    (int)(strpos($verbose, "\ngrpc-status:") === false && strpos($verbose, "< grpc-status") === false), "\n";

    $server->stop();
});

$server->start();
await($client);
echo "Done\n";
--EXPECT--
resp_ctype_webtext=1
body_is_base64=1
data=echo:ping,bye
trailer_status=1
no_http_trailer=1
Done
