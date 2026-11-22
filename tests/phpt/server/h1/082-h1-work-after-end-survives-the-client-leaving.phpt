--TEST--
A client that closes the connection once it has the whole response does not cancel what the handler does after end()
--EXTENSIONS--
true_async_server
true_async
sockets
--FILE--
<?php
/* The response is streamed, so its terminator leaves at end() and the client can
 * close while the handler still runs. Work after end() is the server's own: a framework's terminate step (session save,
 * terminable middleware, jobs dispatched after the response) runs there. The client
 * read the whole answer and left, which is no reason to cancel it. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\HttpException;
use function Async\spawn;
use function Async\await;
use function Async\delay;

require_once __DIR__ . '/../_free_port.inc';

$port = tas_free_port();
$server = new HttpServer(
    (new HttpServerConfig())
        ->addListener('127.0.0.1', $port)
        ->setReadTimeout(10)
        ->setWriteTimeout(10)
);

$server->addHttpHandler(function ($req, $res) use ($server) {
    try {
        $res->write('done');
        $res->end();
        delay(200);
        echo "after end ran\n";
    } catch (HttpException $e) {
        echo "after end cancelled: ", $e->getCode(), "\n";
    } finally {
        $server->stop();
    }
});

$client = spawn(function () use ($port) {
    delay(30);
    $sock = socket_create(AF_INET, SOCK_STREAM, SOL_TCP);
    socket_connect($sock, '127.0.0.1', $port);
    socket_write($sock, "GET / HTTP/1.1\r\nHost: x\r\n\r\n");
    // Up to the terminator: leaving on the body alone races end(), whose
    // write then fails, and on Windows at once.
    $got = '';
    while (!str_contains($got, "\r\n0\r\n\r\n")) {
        $piece = socket_read($sock, 4096);
        if ($piece === false || $piece === '') break;
        $got .= $piece;
    }
    socket_close($sock);
    echo str_contains($got, 'done') ? "client got the answer\n" : "client missed it\n";
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
client got the answer
after end ran
