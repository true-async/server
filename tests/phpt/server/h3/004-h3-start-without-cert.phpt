--TEST--
HttpServer: addHttp3Listener without cert/key — start() fails cleanly
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
if (!class_exists('TrueAsync\HttpServer')) die('skip http_server not loaded');
?>
--FILE--
<?php
/* QUIC mandates TLS 1.3. addHttp3Listener sets tls=true on its listener
 * entry, so start() goes through the same cert/key validation as the
 * TCP+TLS path. With no certificate configured, start() must throw a
 * descriptive exception; the server must stay in a non-running state
 * so a retry after calling setCertificate/setPrivateKey works. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;

$port = 20100 + getmypid() % 50;

$config = (new HttpServerConfig())
    ->addHttp3Listener('127.0.0.1', $port)
    ->setReadTimeout(2);

$server = new HttpServer($config);
$server->addHttpHandler(fn($r, $s) => $s->setStatusCode(200)->setBody('x'));

echo "running_before=", (int)$server->isRunning(), "\n";
try {
    $server->start();
    echo "start returned normally (unexpected)\n";
} catch (Throwable $e) {
    $msg = $e->getMessage();
    echo "start_threw=1\n";
    echo "mentions_cert=", (int)(str_contains($msg, 'cert') || str_contains($msg, 'TLS')), "\n";
}
echo "running_after=", (int)$server->isRunning(), "\n";
echo "done\n";
?>
--EXPECT--
running_before=0
start_threw=1
mentions_cert=1
running_after=0
done
