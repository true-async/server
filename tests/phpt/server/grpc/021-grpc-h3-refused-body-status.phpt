--TEST--
gRPC over HTTP/3: a request body past setMaxBodySize ends with grpc-status 8, not a bare reset (#426)
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
/* A gRPC call whose message is past the body limit was refused with
 * STOP_SENDING and RESET_STREAM(H3_REQUEST_REJECTED): the client read no
 * grpc-status. The refusal now answers RESOURCE_EXHAUSTED (8) in a
 * trailers-only response and stops only the read side. aioquic drives it,
 * since the C h3client reads no trailers. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

$tmp = __DIR__ . '/tmp-grpc-h3-021';
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
    ->setCertificate($cert)->setPrivateKey($key)
    ->setMaxBodySize(64 * 1024)
    ->setReadTimeout(5)->setWriteTimeout(5);

$server = new HttpServer($config);
$server->addGrpcHandler(function ($req, $resp) {
    $req->awaitBody();
    $req->readMessage();
    $resp->writeMessage('unexpected');
});

$py = __DIR__ . '/_h3grpc_client.py';

$client = spawn(function () use ($server, $port, $py) {
    usleep(200000);

    $msg     = str_repeat('a', 200 * 1024);
    $hexfile = tempnam(sys_get_temp_dir(), 'grpch3');
    file_put_contents($hexfile, bin2hex("\x00" . pack('N', strlen($msg)) . $msg));
    $cmd = sprintf('python3 %s 127.0.0.1 %d /svc/Upload application/grpc %s 2>/dev/null',
        escapeshellarg($py), $port, escapeshellarg('@' . $hexfile));
    $out = (string) shell_exec($cmd);
    @unlink($hexfile);

    echo "saw_status_200=", (int)(strpos($out, 'HDR :status: 200') !== false), "\n";
    echo "grpc_status=", preg_match('/^HDR grpc-status: (\d+)$/m', $out, $m) ? $m[1] : 'none', "\n";
    echo "saw_grpc_message=", (int)(strpos($out, 'HDR grpc-message:') !== false), "\n";

    $server->stop();
});

$server->start();
await($client);
echo "Done\n";
--EXPECT--
saw_status_200=1
grpc_status=8
saw_grpc_message=1
Done
