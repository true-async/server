--TEST--
gRPC over HTTP/3 under the reactor pool: unary echo + native trailers (#4 + #80)
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
require __DIR__ . '/../h3/_h3_skipif.inc';
if (PHP_OS_FAMILY === 'Windows') die('skip libuv on Windows lacks SO_REUSEPORT');
h3_skipif(['openssl_cli' => true, 'aioquic' => true]);
?>
--ENV--
TRUE_ASYNC_SERVER_REACTOR_POOL=1
PHP_HTTP3_DISABLE_RETRY=1
--FILE--
<?php
/* gRPC rides the generic pool reverse path: the worker classifies
 * application/grpc (same predicate as the transports), runs the
 * addGrpcHandler callable, writeMessage() streams the reply frame over
 * STREAM_HEADERS/STREAM_CHUNK, and grpc_call_finish's end_stream posts
 * STREAM_END whose trailers (grpc-status) the reactor submits at true
 * EOF. No gRPC-specific code in the reactor/worker split itself. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;

require __DIR__ . '/../h3/_h3_skipif.inc';

$tmp = __DIR__ . '/tmp-grpc-pool';
@mkdir($tmp, 0700, true);
$cert = $tmp . '/cert.pem';
$key  = $tmp . '/key.pem';
if (!h3_gen_cert($key, $cert)) { echo "cert gen failed\n"; exit(1); }
register_shutdown_function(function () use ($tmp, $cert, $key) {
    @unlink($cert); @unlink($key); @rmdir($tmp);
});

require_once __DIR__ . '/../_free_port.inc';

$port = tas_free_port_span(2);
$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port + 1)   /* TCP listener required by start() */
    ->addHttp3Listener('127.0.0.1', $port)
    ->enableTls(true)->setCertificate($cert)->setPrivateKey($key)
    ->setWorkers(2);
$server = new HttpServer($config);
$server->addGrpcHandler(function ($req, $resp) {
    $req->awaitBody();
    $msg = $req->readMessage();
    $resp->writeMessage('echo:' . ($msg ?? ''));
    // grpc-status defaults to 0
});

$py = __DIR__ . '/_h3grpc_client.py';

spawn(function () use ($server, $port, $py) {
    /* Reactors + workers need a moment to thread up and bind. */
    usleep(600000);

    $bodyhex = bin2hex("\x00" . pack('N', 4) . 'ping');
    $cmd = sprintf('python3 %s 127.0.0.1 %d /svc/Echo application/grpc %s 2>/dev/null',
        escapeshellarg($py), $port, $bodyhex);
    $out = shell_exec($cmd) ?? '';

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
?>
--EXPECTF--
%Asaw_status_200=1
saw_ctype=1
saw_grpc_status=1
reply=echo:ping
%A
