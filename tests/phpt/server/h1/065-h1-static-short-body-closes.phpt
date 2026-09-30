--TEST--
HttpServer: a static file body cut short closes the connection instead of serving the pipelined request (plaintext and TLS)
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
require __DIR__ . '/../tls/_tls_skipif.inc';
tls_skipif(['openssl_cli' => true, 'proc_open' => true, 'php_ssl' => true]);
?>
--FILE--
<?php
/* The open-file cache keeps the size it saw for its TTL. Truncating the file
 * after the first request makes the next one declare 2 MiB and find EOF at
 * 4 KiB: plaintext sendfile returns 0, the TLS read returns 0. The peer is
 * then owed 2 MiB - 4 KiB bytes it will never get, so anything the server
 * writes next on the connection is read as body. The connection has to close
 * after the short body; the request pipelined behind it goes unanswered. */

require_once __DIR__ . '/../tls/_tls_skipif.inc';
require_once __DIR__ . '/../_free_port.inc';

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\StaticHandler;
use function Async\spawn;
use function Async\await;

$tmp = __DIR__ . '/tmp-065';
$root = "$tmp/docroot";
@mkdir($root, 0700, true);
$cert = "$tmp/cert.pem";
$key  = "$tmp/key.pem";
if (!tls_gen_cert($key, $cert)) { echo "cert generation failed\n"; exit(1); }

$size = 2 * 1024 * 1024;
$file = "$root/big.bin";

register_shutdown_function(function () use ($tmp, $root, $file) {
    @unlink($file); @unlink("$root/small.txt"); @rmdir($root);
    @unlink("$tmp/cert.pem"); @unlink("$tmp/key.pem"); @rmdir($tmp);
});

require_once __DIR__ . '/_pipeline_probe.inc';

foreach (['tcp', 'ssl'] as $scheme) {
    file_put_contents($file, str_repeat('ABCDEFGHIJKLMNOP', $size / 16));
    file_put_contents("$root/small.txt", 'small');

    $port = tas_free_port();
    $config = (new HttpServerConfig())
        ->addListener('127.0.0.1', $port, $scheme === 'ssl')
        ->setReadTimeout(10)->setWriteTimeout(10);

    if ($scheme === 'ssl') {
        $config->enableTls(true)->setCertificate($cert)->setPrivateKey($key);
    }

    $server = new HttpServer($config);
    $server->addStaticHandler((new StaticHandler('/s/', $root))->setOpenFileCache(16, 60));

    $client = spawn(function () use ($server, $scheme, $port, $file) {
        usleep(100000);
        /* Fills the cache with the 2 MiB size. */
        $warm = h1_pipeline_probe($scheme, $port, '/s/small.txt', '/s/big.bin');
        echo "$scheme warm: ", str_contains($warm, 'extra=200') ? 'served' : $warm, "\n";

        $fp = fopen($file, 'r+');
        ftruncate($fp, 4096);
        fclose($fp);
        clearstatcache();

        echo "$scheme short: ", h1_pipeline_probe($scheme, $port, '/s/big.bin', '/s/small.txt');
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
tcp warm: served
tcp short: status=200 declared=2097152 got=4096 extra=none end=closed
ssl warm: served
ssl short: status=200 declared=2097152 got=4096 extra=none end=closed
