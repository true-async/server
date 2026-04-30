--TEST--
HttpServer: TLS handshake completes (step 4 closes right after)
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
require __DIR__ . '/_tls_skipif.inc';
tls_skipif(['proc_open' => true, 'openssl_cli' => true]);
?>
--FILE--
<?php
require_once __DIR__ . '/_tls_skipif.inc';
use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

// ---- Generate self-signed cert in a temp dir.
$tmp_dir = __DIR__ . '/tmp-061';
if (!is_dir($tmp_dir)) {
    mkdir($tmp_dir, 0700, true);
}
$cert_path = $tmp_dir . '/cert.pem';
$key_path  = $tmp_dir . '/key.pem';
if (!tls_gen_cert($key_path, $cert_path)) {
    echo "cert generation failed
";
    exit(1);
}

$port = 19970 + getmypid() % 20;

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port, true)   // tls=true
    ->enableTls(true)
    ->setCertificate($cert_path)
    ->setPrivateKey($key_path)
    ->setReadTimeout(5)
    ->setWriteTimeout(5);

$server = new HttpServer($config);
$server->addHttpHandler(function ($req, $res) {
    /* Unused on step 4 — handshake closes before we dispatch any
     * HTTP request. Kept in place so the addHttpHandler contract
     * (needed for start()) is satisfied. */
    $res->setStatusCode(200)->setBody('OK')->end();
});

// ---- Client coroutine: `openssl s_client` — standard TLS client
// with machine-readable status. Exits when the server closes.
$client = spawn(function () use ($port, $cert_path) {
    // Let the listener settle.
    usleep(50000);

    /* -brief prints the negotiated protocol + ciphersuite + peer DN
     * on stderr and nothing else. Enough signal to prove the
     * handshake reached TLS_ESTABLISHED; ALPN-driven dispatch is
     * verified once Step 5 wires HTTP through the session. */
    $cmd = sprintf(
        'echo "" | openssl s_client -connect 127.0.0.1:%d ' .
        '-servername localhost -tls1_3 -brief 2>&1',
        $port
    );
    $out = shell_exec($cmd);

    // Tear the server down once the client has finished.
    global $server;
    $server->stop();

    return $out;
});

// Safety net — stop the server even if the client hangs.
spawn(function () use ($server) {
    usleep(1500000);   // 1.5 s
    if ($server->isRunning()) {
        $server->stop();
    }
});

global $server;
$GLOBALS['server'] = $server;
$server->start();

$out = await($client);

// ---- Assertions. -brief emits:
//   Protocol version: TLSv1.3
//   Ciphersuite: TLS_AES_256_GCM_SHA384
//   Hash used: SHA384
echo "tls13: " . (stripos($out, 'TLSv1.3') !== false ? 'yes' : 'no') . "\n";
echo "aead: "  . (stripos($out, 'GCM') !== false ||
                  stripos($out, 'CHACHA20') !== false ? 'yes' : 'no') . "\n";

// cleanup
@unlink($cert_path);
@unlink($key_path);
@rmdir($tmp_dir);

echo "Done\n";
--EXPECT--
tls13: yes
aead: yes
Done
