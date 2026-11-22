--TEST--
A peer that closes mid-body ends a handler waiting in readBody() with HttpException 499 instead of leaving it parked
--EXTENSIONS--
true_async_server
true_async
sockets
--FILE--
<?php
/* A chunked body streams, so the handler starts on the headers and reads the body as it arrives. The client
 * sends a chunked body, one chunk of 10 bytes, and closes. No more bytes are coming, so readBody()
 * must not wait for them: the handler gets HttpException 499 and its finally runs. */

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
        ->setBodyStreamingEnabled(true)
        ->setReadTimeout(30)
        ->setWriteTimeout(30)
);

$server->addHttpHandler(function ($req, $res) use ($server) {
    $started = hrtime(true);
    $read = 0;

    try {
        while (($chunk = $req->readBody()) !== null) {
            $read += strlen($chunk);
            echo "read $read\n";
        }
        echo "body ended after $read bytes\n";
    } catch (HttpException $e) {
        echo "caught HttpException ", $e->getCode(), "\n";
    } catch (Throwable $e) {
        echo "caught ", get_class($e), "\n";
    } finally {
        $ms = (hrtime(true) - $started) / 1e6;
        echo "finally ", $ms < 5000 ? 'in time' : "after {$ms} ms", "\n";
        $server->stop();
    }
});

$client = spawn(function () use ($port) {
    delay(30);
    $sock = socket_create(AF_INET, SOCK_STREAM, SOL_TCP);
    socket_connect($sock, '127.0.0.1', $port);
    socket_write($sock, "POST /upload HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\na\r\n0123456789\r\n");
    delay(100);
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
echo "server survived\n";
?>
--EXPECT--
read 10
caught HttpException 499
finally in time
server survived
