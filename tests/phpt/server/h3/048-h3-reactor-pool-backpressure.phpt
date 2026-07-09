--TEST--
HttpServer: credit backpressure paces a large streamed reply across the split (#80, gated pool)
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
/* Reactor-pool flow control (#80 step 3): the reply is 4 MiB in 64 KiB
 * chunks — four times the worker's 1 MiB in-flight cap — so the producer
 * coroutine MUST park on the credit block and resume as the reactor
 * retires peer-acked bytes. If acks never advanced, the producer would
 * park forever (write timeout) and the body would come back truncated;
 * a byte-exact checksum proves pacing works end to end. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;

require __DIR__ . '/_h3_skipif.inc';

$tmp = __DIR__ . '/tmp-048';
@mkdir($tmp, 0700, true);
$cert = $tmp . '/cert.pem';
$key  = $tmp . '/key.pem';
if (!h3_gen_cert($key, $cert)) { echo "cert gen failed\n"; exit(1); }
register_shutdown_function(function () use ($tmp, $cert, $key) {
    @unlink($cert); @unlink($key); @rmdir($tmp);
});

require_once __DIR__ . '/../_free_port.inc';

$chunks    = 64;
$chunk_len = 65536;

$port = tas_free_port_span(2);
$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port + 1)   /* TCP listener required by start() */
    ->addHttp3Listener('127.0.0.1', $port)
    ->enableTls(true)->setCertificate($cert)->setPrivateKey($key)
    ->setWorkers(2);
$server = new HttpServer($config);
$server->addHttpHandler(function ($req, $res) use ($chunks, $chunk_len) {
    $res->setStatusCode(200)
        ->setHeader('content-type', 'application/octet-stream');

    for ($i = 0; $i < $chunks; $i++) {
        /* Deterministic per-chunk fill so truncation/reorder breaks the hash. */
        $res->send(str_repeat(chr(65 + ($i % 26)), $chunk_len));
    }

    $res->end();
});

$client_bin = __DIR__ . '/../../../h3client/h3client';

spawn(function () use ($server, $port, $client_bin, $chunks, $chunk_len) {
    /* Reactors + workers need a moment to thread up and bind. */
    usleep(600000);

    $cmd = sprintf('H3CLIENT_DEADLINE_MS=20000 %s 127.0.0.1 %d /big GET 2>&1',
        escapeshellarg($client_bin), $port);
    $out = shell_exec($cmd) ?? '';

    $status = null;
    if (preg_match('/^STATUS=(\d+)$/m', $out, $m)) $status = (int)$m[1];
    $body = preg_replace('/^STATUS=\d+\n?/m', '', $out, 1);

    $expect = '';
    for ($i = 0; $i < $chunks; $i++) {
        $expect .= str_repeat(chr(65 + ($i % 26)), $chunk_len);
    }

    echo "status=", $status ?? -1, "\n";
    echo "len_ok=",  (int)(strlen($body) === strlen($expect)), "\n";
    echo "hash_ok=", (int)(hash('xxh128', $body) === hash('xxh128', $expect)), "\n";

    $server->stop();
});

$server->start();
?>
--EXPECTF--
%Astatus=200
len_ok=1
hash_ok=1
%A
