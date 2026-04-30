--TEST--
HttpServer: TLS telemetry counters advance as expected
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
require __DIR__ . '/_tls_skipif.inc';
tls_skipif(['proc_open' => true, 'openssl_cli' => true, 'curl' => true]);
?>
--FILE--
<?php
require_once __DIR__ . '/_tls_skipif.inc';
use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

$tmp = __DIR__ . '/tmp-066';
if (!is_dir($tmp)) mkdir($tmp, 0700, true);
$cert = "$tmp/cert.pem"; $key = "$tmp/key.pem";
if (!tls_gen_cert($key, $cert)) {
    echo "cert generation failed
";
    exit(1);
}

$port = 19820 + getmypid() % 20;

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port, true)
    ->enableTls(true)
    ->setCertificate($cert)
    ->setPrivateKey($key)
    ->setReadTimeout(5)
    ->setWriteTimeout(5);

$server = new HttpServer($config);
$N = 5;
$seen = 0;
$server->addHttpHandler(function ($req, $res) use (&$seen, $server, $N) {
    $seen++;
    $res->setStatusCode(200)
        ->setHeader('Content-Type', 'text/plain')
        ->setBody("resp:" . $req->getUri())
        ->end();
    if ($seen === $N) $server->stop();
});

$client = spawn(function () use ($port, $N) {
    usleep(50000);
    /* N GETs on ONE keep-alive connection — 1 handshake, 5 requests,
     * 5 responses. Use --next to reuse the connection explicitly. */
    $parts = [];
    for ($i = 1; $i <= $N; $i++) {
        $parts[] = ($i === 1 ? '' : '--next ') . '-sk --http1.1 '
                 . 'https://127.0.0.1:' . $port . '/r' . $i;
    }
    shell_exec('curl -m 5 ' . implode(' ', $parts) . ' 2>&1 > ' . tls_dev_null());
});

spawn(function () use ($server) {
    usleep(4_000_000);
    if ($server->isRunning()) $server->stop();
});

$server->start();
await($client);

$t = $server->getTelemetry();

/* Shape assertions. Absolute latencies vary with machine, so we only
 * check that counters moved in the right direction and that ratios
 * hold (cipher bytes out ≥ plaintext out because of TLS overhead,
 * same direction on the receive side). */
echo "handshakes: "     . ($t['tls_handshakes_total'] >= 1 ? 'ok' : 'bad(' . $t['tls_handshakes_total'] . ')') . "\n";
echo "failures: "       . ($t['tls_handshake_failures_total'] === 0 ? 'ok' : 'bad') . "\n";
echo "hs_avg_ms_pos: "  . ($t['tls_handshake_avg_ms'] > 0.0 ? 'ok' : 'bad(' . $t['tls_handshake_avg_ms'] . ')') . "\n";
echo "pt_in_pos: "      . ($t['tls_bytes_plaintext_in_total']  > 0 ? 'ok' : 'bad') . "\n";
echo "pt_out_pos: "     . ($t['tls_bytes_plaintext_out_total'] > 0 ? 'ok' : 'bad') . "\n";
echo "ct_in_pos: "      . ($t['tls_bytes_ciphertext_in_total'] > 0 ? 'ok' : 'bad') . "\n";
echo "ct_out_pos: "     . ($t['tls_bytes_ciphertext_out_total'] > 0 ? 'ok' : 'bad') . "\n";
echo "cipher_overhead_in: "  . ($t['tls_bytes_ciphertext_in_total']  >= $t['tls_bytes_plaintext_in_total']  ? 'ok' : 'bad') . "\n";
echo "cipher_overhead_out: " . ($t['tls_bytes_ciphertext_out_total'] >= $t['tls_bytes_plaintext_out_total'] ? 'ok' : 'bad') . "\n";

/* resetTelemetry zeroes the TLS bucket too. */
$server->resetTelemetry();
$t2 = $server->getTelemetry();
echo "reset_handshakes: "   . ($t2['tls_handshakes_total'] === 0 ? 'ok' : 'bad') . "\n";
echo "reset_pt_bytes: "     . ($t2['tls_bytes_plaintext_in_total']  === 0 &&
                               $t2['tls_bytes_plaintext_out_total'] === 0 ? 'ok' : 'bad') . "\n";

@unlink($cert); @unlink($key); @rmdir($tmp);
echo "Done\n";
--EXPECT--
handshakes: ok
failures: ok
hs_avg_ms_pos: ok
pt_in_pos: ok
pt_out_pos: ok
ct_in_pos: ok
ct_out_pos: ok
cipher_overhead_in: ok
cipher_overhead_out: ok
reset_handshakes: ok
reset_pt_bytes: ok
Done
