--TEST--
HttpServer: streaming send() crosses the reactor/worker split (#80, gated pool)
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
require __DIR__ . '/_h3_skipif.inc';
if (PHP_OS_FAMILY === 'Windows') die('skip libuv on Windows lacks SO_REUSEPORT');
h3_skipif(['openssl_cli' => true, 'h3client' => true]);
?>
--ENV--
TRUE_ASYNC_SERVER_REACTOR_POOL=1
PHP_HTTP3_DISABLE_RETRY=1
--FILE--
<?php
/* Reactor-pool reverse path, streaming leg: HttpResponse::send() on the
 * worker posts STREAM_HEADERS on first call (streaming submit on the
 * reactor), each chunk as STREAM_CHUNK (reactor chunk ring + resume), and
 * end() as STREAM_END (EOF). Before this, send() under the pool threw
 * "streaming not available". Chunk ORDER and CONTENT prove the FIFO wire. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;

require __DIR__ . '/_h3_skipif.inc';

$tmp = __DIR__ . '/tmp-047';
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
        ->setHeader('content-type', 'text/plain; charset=utf-8');

    for ($i = 1; $i <= 5; $i++) {
        $res->send("chunk{$i};");
    }

    $res->end();
});

$client_bin = __DIR__ . '/../../../h3client/h3client';

spawn(function () use ($server, $port, $client_bin) {
    /* Reactors + workers need a moment to thread up and bind. */
    usleep(600000);

    $cmd = sprintf('H3CLIENT_DEADLINE_MS=4000 %s 127.0.0.1 %d /stream GET 2>&1',
        escapeshellarg($client_bin), $port);
    $out = shell_exec($cmd) ?? '';

    $status = null;
    if (preg_match('/^STATUS=(\d+)$/m', $out, $m)) $status = (int)$m[1];
    $body = preg_replace('/^STATUS=\d+\n?/m', '', $out);

    echo "status=", $status ?? -1, "\n";
    echo "body=",   trim($body), "\n";

    /* Issue #11: no clean cross-thread shutdown for the pool yet; SIGKILL
     * skips PHP shutdown so the worker threads cannot deadlock on exit. */
    posix_kill(getmypid(), SIGKILL);
});

$server->start();
?>
--EXPECTF--
%Astatus=200
body=chunk1;chunk2;chunk3;chunk4;chunk5;
%A
