--TEST--
HttpServer: a restarted server drains its in-flight handlers at the second stop() too (#345)
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
/* start() drains the server scope once stop() wakes it: it waits for the
 * handlers still running, within the shutdown grace, and returns after them.
 * The drain belongs to one run, so a second start() on the same server drains
 * too. Each run stops the server while /slow is in flight; the handler
 * finishes before start() returns in both. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;
use function Async\delay;

require_once __DIR__ . '/../_free_port.inc';

$port = tas_free_port();
$server = new HttpServer((new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(10)
    ->setWriteTimeout(10)
    ->setShutdownTimeout(2));

$server->addHttpHandler(function ($req, $res) {
    delay(300);
    echo "handler finished\n";
    $res->setStatusCode(200)->setBody('ok')->end();
});

foreach (['first', 'second'] as $run) {
    $client = spawn(function () use ($port, $server) {
        $c = stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 2);
        fwrite($c, "GET /slow HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n");
        delay(100);
        $server->stop();
        fclose($c);
    });
    $server->start();
    echo "$run: start() returned\n";
    await($client);
}
?>
--EXPECT--
handler finished
first: start() returned
handler finished
second: start() returned
