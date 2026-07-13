--TEST--
StaticHandler over HTTP/3 under the reactor pool: a file past the inline threshold is served by the reactor
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
require __DIR__ . '/_h3_skipif.inc';
if (PHP_OS_FAMILY === 'Windows') die('skip libuv on Windows lacks SO_REUSEPORT');
h3_skipif(['openssl_cli' => true, 'h3client' => true]);
?>
--ENV--
TRUE_ASYNC_SERVER_REACTOR_POOL=1
PHP_HTTP3_DISABLE_RETRY=1
--FILE--
<?php
/* A static mount is served on the transport reactor itself — no worker, no PHP.
 * Files up to the 64 KiB inline threshold are answered from memory, but anything
 * larger goes through the body pump, which used to demand a PHP coroutine and so
 * could not run here at all: the request hung until the client gave up. Same code
 * path as a pooled sendFile, but reached without a handler, which is why it gets
 * its own test.
 *
 * The reactor is also the only accountant for these requests — no worker slot
 * exists — so they must show up under getStats()['reactors']. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\StaticHandler;
use function Async\spawn;

require __DIR__ . '/_h3_skipif.inc';
require_once __DIR__ . '/../_free_port.inc';

$dir = __DIR__ . '/tmp-054';
$root = $dir . '/root';
@mkdir($root, 0700, true);
$cert = $dir . '/cert.pem';
$key  = $dir . '/key.pem';
if (!h3_gen_cert($key, $cert)) { echo "cert gen failed\n"; exit(1); }

$small = str_repeat('s', 1024);                 /* inline path */
$big   = str_repeat('b', 200000);               /* past the 64 KiB slurp threshold */
file_put_contents("$root/small.txt", $small);
file_put_contents("$root/big.txt", $big);

register_shutdown_function(function () use ($dir, $root, $cert, $key) {
    @unlink("$root/small.txt"); @unlink("$root/big.txt");
    @unlink($cert); @unlink($key); @rmdir($root); @rmdir($dir);
});

$port = tas_free_port_span(2);

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port + 1)
    ->addHttp3Listener('127.0.0.1', $port)
    ->enableTls(true)->setCertificate($cert)->setPrivateKey($key)
    ->setStatsEnabled(true)
    ->setWorkers(2);

$server = new HttpServer($config);
$server->addStaticHandler(new StaticHandler('/static/', $root));
$server->addHttpHandler(function ($req, $res) {
    $res->setStatusCode(200)->setBody('dynamic')->end();
});

$client_bin = __DIR__ . '/../../../h3client/h3client';

spawn(function () use ($server, $port, $client_bin, $small, $big) {
    usleep(600000);

    $get = function (string $path) use ($client_bin, $port) {
        $out = sys_get_temp_dir() . '/h3-054-body-' . getmypid();
        $cmd = sprintf('H3CLIENT_DEADLINE_MS=8000 %s 127.0.0.1 %d %s GET 2>&1 >%s',
            escapeshellarg($client_bin), $port, $path, escapeshellarg($out));
        $err  = shell_exec($cmd) ?? '';
        $body = (string)@file_get_contents($out);
        @unlink($out);
        return [preg_match('/STATUS=(\d+)/', $err, $m) ? (int)$m[1] : 0, $body];
    };

    [$st, $body] = $get('/static/small.txt');
    echo "small: status=$st intact=", ($body === $small ? 1 : 0), "\n";

    [$st, $body] = $get('/static/big.txt');
    echo "big:   status=$st intact=", ($body === $big ? 1 : 0),
         " (", strlen($body), " bytes)\n";

    $stats = [];
    for ($i = 0; $i < 60; $i++) {
        $stats = $server->getStats();
        if (($stats['totals']['total_requests'] ?? 0) >= 2) break;
        usleep(50000);
    }

    $t = $stats['totals'];
    $reactor_total = 0;
    foreach ($stats['reactors'] ?? [] as $r) { $reactor_total += $r['total_requests']; }

    echo 'total=',         ($t['total_requests'] === 2 ? 1 : 0), "\n";
    echo 'from_reactors=', ($reactor_total === 2 ? 1 : 0), "\n";
    /* Both were answered without ever entering the PHP coroutine. */
    echo 'hard_zero=',     ($t['static_zero_coroutine_total'] === 2 ? 1 : 0), "\n";
    echo "done\n";

    /* Pool-parent stop() is unsupported (#11); %A absorbs its exception. */
    $server->stop();
});

$server->start();
?>
--EXPECTF--
%Asmall: status=200 intact=1
big:   status=200 intact=1 (200000 bytes)
total=1
from_reactors=1
hard_zero=1
done
%A
