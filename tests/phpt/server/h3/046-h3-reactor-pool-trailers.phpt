--TEST--
HttpServer: response trailers cross the reactor/worker split (#80 + #4, gated pool)
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
require __DIR__ . '/_h3_skipif.inc';
if (PHP_OS_FAMILY === 'Windows') die('skip libuv on Windows lacks SO_REUSEPORT');
h3_skipif(['openssl_cli' => true, 'aioquic' => true]);
?>
--ENV--
TRUE_ASYNC_SERVER_REACTOR_POOL=1
PHP_HTTP3_DISABLE_RETRY=1
--FILE--
<?php
/* Reactor-pool reverse path, trailers leg: a worker-rendered buffered
 * response carries its trailer map over the response_wire; the reactor
 * copies it onto the stream and the data reader submits it via
 * nghttp3_conn_submit_trailers at true EOF (NO_END_STREAM on the final
 * DATA). This is the substrate unary gRPC under the pool rides on —
 * without it grpc-status can never reach the socket. The C h3client
 * can't read H3 trailers, so the aioquic client drives this. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;

require __DIR__ . '/_h3_skipif.inc';

$tmp = __DIR__ . '/tmp-046';
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
$server->addHttpHandler(function ($req, $res) {
    $res->setStatusCode(200)
        ->setHeader('content-type', 'text/plain; charset=utf-8')
        ->setTrailer('x-wire-trailer', 'crossed')
        ->setTrailer('grpc-status', '0')
        ->setBody('pool-body');
});

$py = __DIR__ . '/../grpc/_h3grpc_client.py';

spawn(function () use ($server, $port, $py) {
    /* Reactors + workers need a moment to thread up and bind. */
    usleep(600000);

    $cmd = sprintf("python3 %s 127.0.0.1 %d /trailers text/plain '' 2>/dev/null",
        escapeshellarg($py), $port);
    $out = shell_exec($cmd) ?? '';

    $body = '';
    if (preg_match('/^BODYHEX ([0-9a-f]*)$/m', $out, $m)) {
        $body = hex2bin($m[1]);
    }

    echo "saw_status_200=", (int)(strpos($out, 'HDR :status: 200') !== false), "\n";
    echo "body=", $body, "\n";
    echo "saw_wire_trailer=", (int)(strpos($out, 'HDR x-wire-trailer: crossed') !== false), "\n";
    echo "saw_grpc_status=",  (int)(strpos($out, 'HDR grpc-status: 0') !== false), "\n";

    /* Issue #11: no clean cross-thread shutdown for the pool yet; SIGKILL
     * skips PHP shutdown so the worker threads cannot deadlock on exit. */
    posix_kill(getmypid(), SIGKILL);
});

$server->start();
?>
--EXPECTF--
%Asaw_status_200=1
body=pool-body
saw_wire_trailer=1
saw_grpc_status=1
%A
