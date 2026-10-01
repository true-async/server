--TEST--
HttpServer: a static file body cut short over TLS closes the connection instead of serving the pipelined request
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
 * 4 KiB, where the TLS file read returns 0. The peer is then owed bytes it
 * will never get, so anything the server writes next on the connection is
 * read as body. The connection has to close after the short body; the request
 * pipelined behind it goes unanswered, and the next request for the file is
 * sized from the file again. The plaintext path is 065. */

require_once __DIR__ . '/../tls/_tls_skipif.inc';
require_once __DIR__ . '/../_free_port.inc';

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\StaticHandler;
use function Async\spawn;
use function Async\await;

$tmp = __DIR__ . '/tmp-068';
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

$scheme = 'ssl';
file_put_contents($file, str_repeat('ABCDEFGHIJKLMNOP', $size / 16));
file_put_contents("$root/small.txt", 'small');

$port = tas_free_port();
$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port, true)
    ->setCertificate($cert)->setPrivateKey($key)
    ->setReadTimeout(10)->setWriteTimeout(10);

$server = new HttpServer($config);
$server->addStaticHandler((new StaticHandler('/s/', $root))->setOpenFileCache(16, 60));

$client = spawn(function () use ($server, $scheme, $port, $file) {
    usleep(100000);
    /* Fills the cache with the 2 MiB size. */
    echo "$scheme warm: ", h1_pipeline_probe($scheme, $port, '/s/big.bin', '/s/small.txt');

    $fp = fopen($file, 'r+');
    ftruncate($fp, 4096);
    fclose($fp);
    clearstatcache();

    echo "$scheme short: ", h1_pipeline_probe($scheme, $port, '/s/big.bin', '/s/small.txt');
    /* The failure evicted the stale entry, so the size is taken afresh. */
    echo "$scheme after: ", h1_pipeline_probe($scheme, $port, '/s/big.bin', '/s/small.txt');
    $server->stop();
});

spawn(function () use ($server) {
    usleep(15000000);
    if ($server->isRunning()) { $server->stop(); }
});

$server->start();
await($client);
?>
--EXPECTF--
ssl warm: status=200 declared=2097152 got=2097152 extra=200 end=closed
ssl short: status=200 declared=2097152 got=%d extra=none end=closed
ssl after: status=200 declared=4096 got=4096 extra=200 end=closed
