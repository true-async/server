--TEST--
HttpRequest::isSecure(): true over TLS on HTTP/1.1 and HTTP/2, false on plaintext (#444)
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
/* One server, a TLS listener and a plaintext one. Each request answers with
 * what isSecure() and getHttpVersion() say, so a flag copied from the wrong
 * connection, or not copied on one protocol, shows in the line for it. */
require_once __DIR__ . '/_tls_skipif.inc';
require_once __DIR__ . '/../_free_port.inc';

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

$tmp = __DIR__ . '/tmp-020';
@mkdir($tmp, 0700, true);
$cert = $tmp . '/cert.pem';
$key  = $tmp . '/key.pem';
if (!tls_gen_cert($key, $cert)) { echo "cert generation failed\n"; exit(1); }
register_shutdown_function(function () use ($tmp, $cert, $key) {
    @unlink($cert); @unlink($key); @rmdir($tmp);
});

$tls_port   = tas_free_port();
$plain_port = tas_free_port();
$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $tls_port, true)
    ->addListener('127.0.0.1', $plain_port)
    ->setCertificate($cert)
    ->setPrivateKey($key)
    ->setReadTimeout(5)
    ->setWriteTimeout(5);

$server = new HttpServer($config);
$server->addHttpHandler(function ($req, $res) {
    $res->setStatusCode(200)
        ->setBody(($req->isSecure() ? 'secure' : 'plain') . ' ' . $req->getHttpVersion());
});

$client = spawn(function () use ($server, $tls_port, $plain_port) {
    usleep(50000);
    $runs = [
        'tls h1'   => "--http1.1 https://127.0.0.1:$tls_port/",
        'tls h2'   => "--http2 https://127.0.0.1:$tls_port/",
        'plain h1' => "--http1.1 http://127.0.0.1:$plain_port/",
        'plain h2' => "--http2-prior-knowledge http://127.0.0.1:$plain_port/",
    ];
    foreach ($runs as $label => $args) {
        echo $label, ': ', shell_exec("curl -sk -m 4 $args 2>&1"), "\n";
    }
    $server->stop();
});

spawn(function () use ($server) {
    usleep(5000000);
    if ($server->isRunning()) $server->stop();
});

$server->start();
await($client);
echo "Done\n";
?>
--EXPECT--
tls h1: secure 1.1
tls h2: secure 2.0
plain h1: plain 1.1
plain h2: plain 2.0
Done
