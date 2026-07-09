--TEST--
gRPC-Web over HTTP/3: trailers in-body as a 0x80 frame (real aioquic client)
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
require __DIR__ . '/../h3/_h3_skipif.inc';
h3_skipif(['openssl_cli' => true, 'aioquic' => true]);
?>
--FILE--
<?php
/* grpc-web over H3: the response content-type is application/grpc-web+proto and
 * the grpc-status/grpc-message trailers ride the body as a 0x80-flagged frame
 * (not HTTP/3 trailers) — same finalize as H2 grpc-web, on the H3 body path. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

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

$tmp = __DIR__ . '/tmp-grpcweb-h3';
@mkdir($tmp, 0700, true);
$cert = $tmp . '/cert.pem'; $key = $tmp . '/key.pem'; $rc = 0;
exec(sprintf('openssl req -x509 -newkey rsa:2048 -sha256 -days 1 -nodes '
    . '-subj "/CN=localhost" -keyout %s -out %s 2>/dev/null',
    escapeshellarg($key), escapeshellarg($cert)), $_, $rc);
if ($rc !== 0) { echo "cert gen failed\n"; exit(1); }
register_shutdown_function(function () use ($tmp) {
    @unlink("$tmp/cert.pem"); @unlink("$tmp/key.pem"); @rmdir($tmp);
});

require_once __DIR__ . '/../_free_port.inc';

$port = tas_free_port_span(2);
$server = new HttpServer((new HttpServerConfig())
    ->addListener('127.0.0.1', $port + 1)
    ->addHttp3Listener('127.0.0.1', $port)
    ->enableTls(true)->setCertificate($cert)->setPrivateKey($key)
    ->setReadTimeout(5)->setWriteTimeout(5));

$server->addGrpcHandler(function ($req, $resp) {
    $req->awaitBody();
    $msg = $req->readMessage();
    $resp->writeMessage('echo:' . ($msg ?? ''));
});

$py = __DIR__ . '/_h3grpc_client.py';

$client = spawn(function () use ($server, $port, $py) {
    usleep(200000);
    $bodyhex = bin2hex("\x00" . pack('N', 4) . 'ping');
    $cmd = sprintf('python3 %s 127.0.0.1 %d /svc/Echo application/grpc-web+proto %s 2>/dev/null',
        escapeshellarg($py), $port, $bodyhex);
    $out = shell_exec($cmd);

    $body = '';
    if (preg_match('/^BODYHEX ([0-9a-f]*)$/m', $out, $m)) {
        $body = hex2bin($m[1]);
    }
    [$data, $trailers] = grpcweb_parse($body);

    echo "saw_ctype_web=",  (int)(strpos($out, 'HDR content-type: application/grpc-web+proto') !== false), "\n";
    echo "data=",           implode(',', $data), "\n";
    echo "trailer_status=", (int)(strpos($trailers, 'grpc-status: 0') !== false), "\n";

    $server->stop();
});

$server->start();
await($client);
echo "Done\n";
--EXPECT--
saw_ctype_web=1
data=echo:ping
trailer_status=1
Done
