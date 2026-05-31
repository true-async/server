--TEST--
HttpServer: HTTP/3 oversized request body (>16 MiB) rejects the stream, connection survives (#59)
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
require __DIR__ . '/_h3_skipif.inc';
h3_skipif(['openssl_cli' => true, 'h3client' => true]);
?>
--FILE--
<?php
/* Test 021 covers the header-bytes cap; this covers the separate body-bytes
 * cap (HTTP3_MAX_BODY_BYTES = 16 MiB). h3_recv_data_cb trips
 * h3_request_oversized + rejects the stream (STOP_SENDING/RESET_STREAM) when
 * the uploaded body crosses the cap — a stream-level reject; the connection
 * must survive. The reject also wakes a handler suspended in awaitBody()
 * (regression guard for the stream-slot leak that fix introduced).
 *
 * The stream window is 20 MiB (> the 16 MiB body cap) so the whole 17 MiB
 * upload fits in one window and the cap trips cleanly — h3client is
 * single-shot and does not resume a flow-control-blocked upload. */

require __DIR__ . '/_h3_skipif.inc';

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

$tmp = __DIR__ . '/tmp-035';
@mkdir($tmp, 0700, true);
$cert = $tmp . '/cert.pem';
$key  = $tmp . '/key.pem';
$big  = $tmp . '/big.bin';
if (!h3_gen_cert($key, $cert)) { echo "cert gen failed\n"; exit(1); }
file_put_contents($big, str_repeat('A', 17 * 1024 * 1024));   /* > 16 MiB */
register_shutdown_function(function () use ($tmp, $cert, $key, $big) {
    @unlink($cert); @unlink($key); @unlink($big); @rmdir($tmp);
});

$port = 20720 + getmypid() % 40;

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port + 1)
    ->addHttp3Listener('127.0.0.1', $port)
    ->enableTls(true)->setCertificate($cert)->setPrivateKey($key)
    ->setHttp3StreamWindowBytes(20 * 1024 * 1024);   /* > 16 MiB body cap */
$server = new HttpServer($config);
$server->addHttpHandler(function ($req, $res) {
    try { $req->awaitBody(); } catch (\Throwable $e) { /* rejected stream */ }
    $res->setStatusCode(200)->setBody('ok');
});

$client_bin = __DIR__ . '/../../../h3client/h3client';

$client = spawn(function () use ($server, $port, $client_bin, $big) {
    usleep(120000);

    /* Request 1: POST the oversized body — expected to trip the cap. */
    shell_exec(sprintf('%s 127.0.0.1 %d /upload POST %s 2>&1',
        escapeshellarg($client_bin), $port, escapeshellarg($big)));

    $st = $server->getHttp3Stats()[0] ?? [];
    echo "oversized_ge1=", ((int)($st['h3_request_oversized'] ?? 0) >= 1 ? 1 : 0), "\n";

    /* Request 2: a fresh GET — proves the server survived the reject. */
    $out = shell_exec(sprintf('%s 127.0.0.1 %d / GET 2>&1',
        escapeshellarg($client_bin), $port)) ?? '';
    $status = preg_match('/^STATUS=(\d+)$/m', $out, $m) ? (int)$m[1] : -1;
    echo "survived_status=", $status, "\n";

    $server->stop();
});

$server->start();
await($client);
echo "done\n";
?>
--EXPECT--
oversized_ge1=1
survived_status=200
done
