--TEST--
A client that pipelines two requests and shuts its write half gets both answers: a FIN with requests buffered is a half-close
--EXTENSIONS--
true_async_server
true_async
sockets
--FILE--
<?php
/* The peer sends two requests in one write and shuts down its write half, the way
 * `nc -N` and scripted HTTP/1.0 probes do, then reads. The first handler is still
 * waiting when the FIN arrives, with the second request in the buffer: that peer is
 * still reading, so neither handler is cancelled and both responses arrive. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
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

$server->addHttpHandler(function ($req, $res) {
    if ($req->getPath() === '/slow') {
        delay(300);
    }
    $res->setBody('answer' . $req->getPath())->end();
});

$client = spawn(function () use ($port, $server) {
    delay(30);
    $sock = socket_create(AF_INET, SOCK_STREAM, SOL_TCP);
    socket_connect($sock, '127.0.0.1', $port);
    socket_write($sock, "GET /slow HTTP/1.1\r\nHost: x\r\n\r\nGET /fast HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
    socket_shutdown($sock, 1);

    $got = '';
    while (($piece = socket_read($sock, 4096)) !== false && $piece !== '') {
        $got .= $piece;
    }
    socket_close($sock);

    echo str_contains($got, 'answer/slow') ? "slow answered\n" : "slow missing\n";
    echo str_contains($got, 'answer/fast') ? "fast answered\n" : "fast missing\n";
    $server->stop();
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
slow answered
fast answered
