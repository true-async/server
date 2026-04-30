--TEST--
HttpServer: TLS listener accepts cert/key; missing cert throws
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

// Generate a throwaway self-signed certificate in the test tmp dir.
$tmp_dir = __DIR__ . '/tmp-060';
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

$port = 19960 + getmypid() % 30;

// --- Case 1: TLS listener with valid cert/key starts and stops cleanly.
$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port, true)   // tls=true
    ->enableTls(true)
    ->setCertificate($cert_path)
    ->setPrivateKey($key_path)
    ->setReadTimeout(5)
    ->setWriteTimeout(5);

echo "isTlsEnabled: " . ($config->isTlsEnabled() ? 'yes' : 'no') . "\n";
echo "cert: " . basename($config->getCertificate()) . "\n";
echo "key: "  . basename($config->getPrivateKey())  . "\n";

$server = new HttpServer($config);
$server->addHttpHandler(function ($req, $res) {
    $res->setStatusCode(200)->setBody('OK')->end();
});

spawn(function () use ($server) {
    usleep(50000);
    $server->stop();
});

echo "starting...\n";
$server->start();
echo "stopped\n";

// --- Case 2: TLS listener but no cert configured → start() throws.
$config2 = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port + 1, true)
    ->enableTls(true)
    ->setReadTimeout(5);

$server2 = new HttpServer($config2);
$server2->addHttpHandler(function ($req, $res) {
    $res->setStatusCode(200)->setBody('OK')->end();
});

try {
    $server2->start();
    echo "no exception (unexpected)\n";
} catch (Throwable $e) {
    echo "caught: " . (str_contains($e->getMessage(), 'certificate') ? 'cert-related' : 'other') . "\n";
}

// cleanup
@unlink($cert_path);
@unlink($key_path);
@rmdir($tmp_dir);

echo "Done\n";
--EXPECT--
isTlsEnabled: yes
cert: cert.pem
key: key.pem
starting...
stopped
caught: cert-related
Done
