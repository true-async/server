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
use function Async\delay;

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

require_once __DIR__ . '/../_free_port.inc';

$port = tas_free_port();
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
// The listener is bound before start() suspends, and this coroutine first
// runs at that suspend, so the connect needs no head start.
$client = spawn(function () use ($port, $cert_path, $tmp_dir) {
    /* -brief prints the negotiated protocol + ciphersuite + peer DN
     * on stderr and nothing else. Enough signal to prove the
     * handshake reached TLS_ESTABLISHED; ALPN-driven dispatch is
     * verified once Step 5 wires HTTP through the session.
     *
     * s_client leaves once the server closes. An HTTP/1.0 request on its
     * stdin gets one answer and the close; an empty stdin does not do it on
     * Windows, where s_client did not see the pipe end and ran until the
     * server was stopped under it. A file redirect reads the same in cmd.exe
     * and sh. */
    $request = $tmp_dir . '/request.txt';
    file_put_contents($request, "GET / HTTP/1.0\r\nHost: localhost\r\n\r\n");
    $cmd = sprintf(
        'openssl s_client -connect 127.0.0.1:%d ' .
        '-servername localhost -tls1_3 -brief < %s 2>&1',
        $port, escapeshellarg($request)
    );
    $out = shell_exec($cmd);
    @unlink($request);

    // Tear the server down once the client has finished.
    global $server;
    $server->stop();

    return $out;
});

// Safety net — stop the server if the client hangs. The client stops it
// itself when s_client exits, which can take seconds where process start is
// slow, so this fires only on a hang, and says so.
$watchdog = spawn(function () use ($server) {
    delay(10000);
    echo "FAIL: s_client still running after 10 s\n";
    $server->stop();
});

global $server;
$GLOBALS['server'] = $server;
$server->start();

$out = await($client);
$watchdog->cancel();

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
