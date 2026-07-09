--TEST--
gRPC over HTTP/3: unary echo — native trailers via nghttp3 (real aioquic client)
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
/* End-to-end gRPC over HTTP/3: same addGrpcHandler / readMessage /
 * writeMessage API as H2, but grpc-status/grpc-message go out through
 * nghttp3_conn_submit_trailers (NGHTTP3_DATA_FLAG_NO_END_STREAM at EOF). The
 * bundled C h3client can't read HTTP/3 trailers, so this is driven by the
 * aioquic client (_h3grpc_client.py), which reports every header + trailer. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

$tmp = __DIR__ . '/tmp-grpc-h3';
@mkdir($tmp, 0700, true);
$cert = $tmp . '/cert.pem';
$key  = $tmp . '/key.pem';
$rc = 0;
exec(sprintf('openssl req -x509 -newkey rsa:2048 -sha256 -days 1 -nodes '
    . '-subj "/CN=localhost" -keyout %s -out %s 2>/dev/null',
    escapeshellarg($key), escapeshellarg($cert)), $_, $rc);
if ($rc !== 0) { echo "cert gen failed\n"; exit(1); }

register_shutdown_function(function () use ($tmp) {
    @unlink("$tmp/cert.pem"); @unlink("$tmp/key.pem"); @rmdir($tmp);
});

require_once __DIR__ . '/../_free_port.inc';

$port = tas_free_port_span(2);
$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port + 1)
    ->addHttp3Listener('127.0.0.1', $port)
    ->enableTls(true)->setCertificate($cert)->setPrivateKey($key)
    ->setReadTimeout(5)->setWriteTimeout(5);

$server = new HttpServer($config);
$server->addGrpcHandler(function ($req, $resp) {
    $req->awaitBody();
    $msg = $req->readMessage();
    $resp->writeMessage('echo:' . ($msg ?? ''));
    // grpc-status defaults to 0
});

$py = __DIR__ . '/_h3grpc_client.py';

$client = spawn(function () use ($server, $port, $py) {
    usleep(200000);

    $bodyhex = bin2hex("\x00" . pack('N', 4) . 'ping');
    $cmd = sprintf('python3 %s 127.0.0.1 %d /svc/Echo application/grpc %s 2>/dev/null',
        escapeshellarg($py), $port, $bodyhex);
    $out = shell_exec($cmd);

    /* Deframe the single response message from the BODYHEX line. */
    $reply = '';
    if (preg_match('/^BODYHEX ([0-9a-f]*)$/m', $out, $m) && strlen($m[1]) >= 10) {
        $body = hex2bin($m[1]);
        $len  = unpack('N', substr($body, 1, 4))[1];
        $reply = substr($body, 5, $len);
    }

    echo "saw_status_200=",  (int)(strpos($out, 'HDR :status: 200') !== false), "\n";
    echo "saw_ctype=",       (int)(strpos($out, 'HDR content-type: application/grpc') !== false), "\n";
    echo "saw_grpc_status=", (int)(strpos($out, 'HDR grpc-status: 0') !== false), "\n";
    echo "reply=", $reply, "\n";

    $server->stop();
});

$server->start();
await($client);
echo "Done\n";
--EXPECT--
saw_status_200=1
saw_ctype=1
saw_grpc_status=1
reply=echo:ping
Done
