--TEST--
A peer that resets or closes the connection cancels a handler suspended after its first chunk with HttpException 499
--EXTENSIONS--
true_async_server
true_async
sockets
--FILE--
<?php
/* The handler writes one chunk and waits far longer than the test runs, the way an
 * SSE loop that only sends on new events waits. The client reads the chunk and goes:
 * /reset with a RST (SO_LINGER {1,0}), /fin with a clean close. Either way the wait
 * must end with HttpException 499 and the handler's finally must run, well before the
 * safety stop. Without the cancel the handler only learns of the departure on its next
 * write, which never comes. */

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
        ->setReadTimeout(30)
        ->setWriteTimeout(30)
);

$done = 0;

$server->addHttpHandler(function ($req, $res) use ($server, &$done) {
    $path = $req->getPath();
    $started = hrtime(true);

    try {
        $res->write("first\n");
        delay(20000);
        echo "$path: woke up\n";
    } catch (HttpException $e) {
        echo "$path: caught HttpException ", $e->getCode(), "\n";
    } finally {
        $ms = (hrtime(true) - $started) / 1e6;
        echo "$path: finally ", $ms < 5000 ? 'in time' : "after {$ms} ms", "\n";

        if (++$done === 2) {
            $server->stop();
        }
    }
});

$client = spawn(function () use ($port) {
    foreach (['/reset', '/fin'] as $path) {
        delay(30);
        $sock = socket_create(AF_INET, SOCK_STREAM, SOL_TCP);
        socket_connect($sock, '127.0.0.1', $port);
        socket_write($sock, "GET $path HTTP/1.1\r\nHost: x\r\n\r\n");
        $got = '';
        while (!str_contains($got, "first")) {
            $piece = socket_read($sock, 4096);
            if ($piece === false || $piece === '') break;
            $got .= $piece;
        }
        if ($path === '/reset') {
            socket_set_option($sock, SOL_SOCKET, SO_LINGER, ['l_onoff' => 1, 'l_linger' => 0]);
        }
        socket_close($sock);
        delay(200);
    }
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
/reset: caught HttpException 499
/reset: finally in time
/fin: caught HttpException 499
/fin: finally in time
server survived
