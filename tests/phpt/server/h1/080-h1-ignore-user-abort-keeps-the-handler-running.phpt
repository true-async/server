--TEST--
setIgnoreUserAbort(true): a peer that leaves does not cancel the handler, which runs to its end and gets 499 on its next write
--EXTENSIONS--
true_async_server
true_async
sockets
--FILE--
<?php
/* The opt-out of h1/077, as PHP's ignore_user_abort(): the handler sleeps through
 * the client's departure and finds out on the write after it. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\HttpException;
use function Async\spawn;
use function Async\await;
use function Async\delay;

require_once __DIR__ . '/../_free_port.inc';

$port = tas_free_port();
$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setIgnoreUserAbort(true)
    ->setReadTimeout(30)
    ->setWriteTimeout(30);
var_dump($config->getIgnoreUserAbort());
$server = new HttpServer($config);

$server->addHttpHandler(function ($req, $res) use ($server) {
    try {
        $res->write("first\n");
        delay(500);
        echo "slept through\n";
        $res->write("second\n");
        echo "second written\n";
    } catch (HttpException $e) {
        echo "write caught HttpException ", $e->getCode(), "\n";
    } finally {
        $server->stop();
    }
});

$client = spawn(function () use ($port) {
    delay(30);
    $sock = socket_create(AF_INET, SOCK_STREAM, SOL_TCP);
    socket_connect($sock, '127.0.0.1', $port);
    socket_write($sock, "GET / HTTP/1.1\r\nHost: x\r\n\r\n");
    socket_read($sock, 4096);
    socket_set_option($sock, SOL_SOCKET, SO_LINGER, ['l_onoff' => 1, 'l_linger' => 0]);
    socket_close($sock);
});

spawn(function () use ($server) {
    delay(8000);
    if ($server->isRunning()) {
        echo "safety stop\n";
        $server->stop();
    }
});

$server->start();
await($client);
?>
--EXPECT--
bool(true)
slept through
write caught HttpException 499
