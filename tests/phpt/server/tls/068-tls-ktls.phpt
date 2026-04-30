--TEST--
HttpServer: kTLS engagement telemetry (observational)
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
require __DIR__ . '/_tls_skipif.inc';
tls_skipif(['proc_open' => true, 'openssl_cli' => true, 'linux_only' => true]);
?>
--FILE--
<?php
require_once __DIR__ . '/_tls_skipif.inc';
use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

$tmp = __DIR__ . '/tmp-068';
if (!is_dir($tmp)) mkdir($tmp, 0700, true);
$cert = "$tmp/cert.pem"; $key = "$tmp/key.pem";
if (!tls_gen_cert($key, $cert)) {
    echo "cert generation failed
";
    exit(1);
}

$port = 19780 + getmypid() % 20;

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port, true)
    ->enableTls(true)
    ->setCertificate($cert)
    ->setPrivateKey($key)
    ->setReadTimeout(5)
    ->setWriteTimeout(5);

$server = new HttpServer($config);
$server->addHttpHandler(function ($req, $res) use ($server) {
    /* Serve a >=16 KiB body — kTLS_sendfile / kTLS_TX is typically
     * tested against workloads large enough for offload to matter. */
    $res->setStatusCode(200)->setBody(str_repeat('x', 32 * 1024))->end();
    $server->stop();
});

$client = spawn(function () use ($port) {
    usleep(60000);
    $cmd = sprintf(
        '(printf "GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n" '
        . '| openssl s_client -connect 127.0.0.1:%d -servername localhost '
        . '-quiet -ign_eof ' . (PHP_OS_FAMILY === 'Windows' ? '2>nul' : '2>/dev/null') . ') || true',
        $port
    );
    shell_exec($cmd);
});

spawn(function () use ($server) {
    usleep(3_000_000);
    if ($server->isRunning()) $server->stop();
});

$server->start();
await($client);

$t = $server->getTelemetry();

/* Assert the counters EXIST and are sane (non-negative zend_long's).
 * Their value depends on the runtime kernel / OpenSSL build:
 *   - kernel >= 5.2 + tls module loaded + OpenSSL ktls build + BIO
 *     pair compatible → >0
 *   - anything missing → 0 (silent fallback)
 * Either is a valid outcome; the test verifies we PROBE correctly. */
echo "has_tx_key: " . (array_key_exists('tls_ktls_tx_total', $t) ? 'ok' : 'bad') . "\n";
echo "has_rx_key: " . (array_key_exists('tls_ktls_rx_total', $t) ? 'ok' : 'bad') . "\n";
echo "tx_non_neg: " . ($t['tls_ktls_tx_total'] >= 0 ? 'ok' : 'bad') . "\n";
echo "rx_non_neg: " . ($t['tls_ktls_rx_total'] >= 0 ? 'ok' : 'bad') . "\n";
echo "hs_ok: "      . ($t['tls_handshakes_total'] >= 1 ? 'ok' : 'bad') . "\n";
@unlink($cert); @unlink($key); @rmdir($tmp);
echo "Done\n";
--EXPECT--
has_tx_key: ok
has_rx_key: ok
tx_non_neg: ok
rx_non_neg: ok
hs_ok: ok
Done
