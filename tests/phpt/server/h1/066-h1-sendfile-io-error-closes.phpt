--TEST--
HttpServer: a sendFile() body that ends in an I/O error closes the connection instead of serving the pipelined request (plaintext and TLS)
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
if (!function_exists('_http_fault_enable')) die('skip needs --enable-fault-injection');
require __DIR__ . '/../tls/_tls_skipif.inc';
tls_skipif(['openssl_cli' => true, 'proc_open' => true, 'php_ssl' => true]);
?>
--FILE--
<?php
/* The fault point fails the transfer where the kernel or the file would: at
 * the plaintext sendfile completion, after the whole body left, and at the
 * first TLS file read, before any of it did. Either way the body the peer got
 * is not the one the server meant, so the connection closes and the request
 * pipelined behind it goes unanswered. */

require_once __DIR__ . '/../tls/_tls_skipif.inc';
require_once __DIR__ . '/../_free_port.inc';
require_once __DIR__ . '/_pipeline_probe.inc';

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

$tmp = __DIR__ . '/tmp-066';
@mkdir($tmp, 0700, true);
$cert = "$tmp/cert.pem";
$key  = "$tmp/key.pem";
if (!tls_gen_cert($key, $cert)) { echo "cert generation failed\n"; exit(1); }

$file = "$tmp/big.bin";
file_put_contents($file, str_repeat('ABCDEFGHIJKLMNOP', 2 * 1024 * 1024 / 16));

register_shutdown_function(function () use ($tmp, $file) {
    @unlink($file); @unlink("$tmp/cert.pem"); @unlink("$tmp/key.pem"); @rmdir($tmp);
});

$point = 'h1/file_body/io_error';

foreach (['tcp', 'ssl'] as $scheme) {
    $port = tas_free_port();
    $config = (new HttpServerConfig())
        ->addListener('127.0.0.1', $port, $scheme === 'ssl')
        ->setReadTimeout(10)->setWriteTimeout(10);

    if ($scheme === 'ssl') {
        $config->setCertificate($cert)->setPrivateKey($key);
    }

    $server = new HttpServer($config);
    $server->addHttpHandler(function ($req, $res) use ($file) {
        $req->getPath() === '/f' ? $res->sendFile($file) : $res->setBody('small');
    });

    $client = spawn(function () use ($server, $scheme, $port, $point) {
        usleep(100000);
        _http_fault_enable($point);
        echo "$scheme: ", h1_pipeline_probe($scheme, $port, '/f', '/small');
        echo "$scheme hits: ", _http_fault_hits($point), "\n";
        $server->stop();
    });

    spawn(function () use ($server) {
        usleep(15000000);
        if ($server->isRunning()) { $server->stop(); }
    });

    $server->start();
    await($client);
}
?>
--EXPECT--
tcp: status=200 declared=2097152 got=2097152 extra=none end=closed
tcp hits: 1
ssl: status=200 declared=2097152 got=0 extra=none end=closed
ssl hits: 1
