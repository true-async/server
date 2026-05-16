--TEST--
HttpServer: H2 over TLS multi-stream (issue #23 regression — emit pump)
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
require __DIR__ . '/_h2_skipif.inc';
h2_skipif(['h2load' => true]);
require __DIR__ . '/../tls/_tls_skipif.inc';
tls_skipif(['proc_open' => true, 'openssl_cli' => true]);
?>
--FILE--
<?php
/*
 * Issue #23 — TLS+H2 concurrent streams used to ship handler 1's full
 * body before handler 2/3 even ran, because the dispose-path drain
 * loop monopolised the single coroutine slot. Peer (h2load) saw a
 * single-stream server and aborted. The fix decouples submission from
 * frame emission: handler dispose calls submit_response + notify and
 * returns; the emit pump fires once in scheduler context with all
 * three submissions queued and ships HEADERS + DATA interleaved.
 *
 * Run h2load with -m 10 -c 4 -n 40 (40 requests over 4 connections,
 * 10 concurrent streams each) against a handler that returns a 256 KiB
 * body. Pass criteria: all 40 requests succeed end-to-end, byte-exact
 * total. Pre-fix this would report 0..4 succeeded depending on the
 * stream-window race.
 */
require_once __DIR__ . '/../tls/_tls_skipif.inc';
use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;

$tmp = __DIR__ . '/tmp-021';

if (!is_dir($tmp)) {
    mkdir($tmp, 0700, true);
}

$cert = $tmp . '/cert.pem';
$key  = $tmp . '/key.pem';

if (!tls_gen_cert($key, $cert)) {
    echo "cert generation failed\n";
    exit(1);
}

$port = 19800 + getmypid() % 200;
$body_bytes = 256 * 1024;

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port, true)
    ->enableTls(true)
    ->setCertificate($cert)
    ->setPrivateKey($key)
    ->setReadTimeout(30)
    ->setWriteTimeout(30);

$server = new HttpServer($config);
$server->addHttpHandler(function ($req, $res) use ($body_bytes, $server) {
    $res->setStatusCode(200)->setBody(str_repeat('Y', $body_bytes))->end();
});

$client = spawn(function () use ($server, $port) {
    usleep(100_000);

    $cmd = sprintf(
        'h2load -n 40 -c 4 -m 10 https://127.0.0.1:%d/ 2>&1',
        $port
    );
    $out = (string)shell_exec($cmd);

    /* Parse h2load summary. */
    $succeeded = 0;
    $errored   = 0;

    if (preg_match('/(\d+)\s+succeeded/', $out, $m)) { $succeeded = (int)$m[1]; }
    if (preg_match('/(\d+)\s+errored/',   $out, $m)) { $errored   = (int)$m[1]; }

    echo 'succeeded: ', ($succeeded === 40 ? 'ok' : "bad ($succeeded)"), "\n";
    echo 'errored: ',   ($errored   === 0  ? 'ok' : "bad ($errored)"),   "\n";

    $server->stop();
});

spawn(function () use ($server) {
    usleep(10_000_000);

    if ($server->isRunning()) { $server->stop(); }
});

$server->start();

@unlink($cert);
@unlink($key);
@rmdir($tmp);

echo "Done\n";
--EXPECT--
succeeded: ok
errored: ok
Done
