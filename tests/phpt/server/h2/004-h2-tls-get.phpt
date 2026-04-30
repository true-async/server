--TEST--
HttpServer: HTTP/2 over TLS via ALPN — GET round-trip
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
require __DIR__ . '/_h2_skipif.inc';
h2_skipif(['curl_h2' => true, 'openssl_cli' => true]);
?>
--FILE--
<?php
/* TLS handshake picks "h2" out of our ALPN preference list; the
 * connection layer sees alpn_selected=H2 and installs the HTTP/2
 * strategy before the first byte of plaintext is read — no preface
 * detection needed. curl --http2 (default ALPN path) drives the
 * whole stack. */

require_once __DIR__ . '/_h2_skipif.inc';

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

$tmp_dir = __DIR__ . '/tmp-073';
if (!is_dir($tmp_dir)) { mkdir($tmp_dir, 0700, true); }
$cert = $tmp_dir . '/cert.pem';
$key  = $tmp_dir . '/key.pem';
if (!h2_gen_cert($key, $cert)) { echo "cert gen failed\n"; exit(1); }

$port = 19970 + getmypid() % 20;

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port, true)
    ->enableTls(true)
    ->setCertificate($cert)
    ->setPrivateKey($key)
    ->setReadTimeout(5)
    ->setWriteTimeout(5);

$server = new HttpServer($config);
$server->addHttpHandler(function ($req, $res) use ($server) {
    $res->setStatusCode(200)
        ->setHeader('Content-Type', 'text/plain')
        ->setBody('hi-over-h2-tls');
    $server->stop();
});

$client = spawn(function () use ($port) {
    usleep(50000);
    /* --http2 without --http2-prior-knowledge = standard ALPN path.
     * -k skips self-signed cert verification. %{http_version}
     * confirms the negotiated protocol was HTTP/2. */
    $cmd = sprintf(
        'curl -sk --http2 -m 4 -o /dev/null '
        . '-w "HTTP=%%{http_code} V=%%{http_version}" '
        . 'https://127.0.0.1:%d/ 2>&1',
        $port
    );
    return shell_exec($cmd);
});

spawn(function () use ($server) {
    usleep(2000000);
    if ($server->isRunning()) $server->stop();
});

$server->start();
$meta = await($client);

echo "status: "  . (strpos($meta, 'HTTP=200') !== false ? 'ok' : 'bad') . "\n";
echo "version: " . (strpos($meta, 'V=2')     !== false ? 'ok' : 'bad') . "\n";

@unlink($cert); @unlink($key);
@rmdir($tmp_dir);
--EXPECT--
status: ok
version: ok
