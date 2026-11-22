--TEST--
setIgnoreUserAbort(true): a peer that closes mid-body still ends readBody(), which throws instead of waiting
--EXTENSIONS--
true_async_server
true_async
sockets
--FILE--
<?php
/* h1/078 with the opt-out: the handler is not cancelled, but the rest of the body
 * is never coming, so readBody() throws rather than leaving the handler parked. */

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
        ->setIgnoreUserAbort(true)
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
        echo "caught ", get_class($e), ": ", $e->getMessage(), "\n";
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
caught Exception: Request body stream error
finally in time
server survived
