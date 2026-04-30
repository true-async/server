--TEST--
HttpServer: TLS security_level=2 rejects RSA-1024 / SHA1 self-signed cert
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

$tmp_dir = __DIR__ . '/tmp-070';
if (!is_dir($tmp_dir)) {
    mkdir($tmp_dir, 0700, true);
}
$cert_path = $tmp_dir . '/cert.pem';
$key_path  = $tmp_dir . '/key.pem';

if (!tls_gen_cert($key_path, $cert_path, 1024, 'sha1')) {
    echo "cert generation failed
";
    exit(1);
}

$port = 19990 + getmypid() % 20;
$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port, true)
    ->enableTls(true)
    ->setCertificate($cert_path)
    ->setPrivateKey($key_path)
    ->setReadTimeout(5);

$server = new HttpServer($config);
$server->addHttpHandler(function ($req, $res) {
    $res->setStatusCode(200)->setBody('OK')->end();
});

try {
    $server->start();
    echo "no exception (unexpected)\n";
} catch (Throwable $e) {
    echo "rejected: yes\n";
}

@unlink($cert_path);
@unlink($key_path);
@rmdir($tmp_dir);
echo "Done\n";
--EXPECTF--
rejected: yes
Done
