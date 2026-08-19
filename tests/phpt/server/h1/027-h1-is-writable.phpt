--TEST--
HttpResponse::isWritable() — false once the peer is gone, and after end(); unlike sendable() it does not flip back
--EXTENSIONS--
true_async_server
true_async
sockets
--FILE--
<?php
/* isWritable() answers "can this response still produce output", which
 * sendable() never did: sendable() also returns false on a full queue, so a
 * handler could not tell "yield and keep streaming" from "stop".
 *
 * On HTTP/1 a departed peer becomes visible only when a write fails, so the
 * handler here writes until it catches the 499 and then asks again. The
 * answer must have flipped to false and must stay false. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\HttpException;
use function Async\spawn;

require_once __DIR__ . '/../_free_port.inc';

$port = tas_free_port();
$server = new HttpServer(
    (new HttpServerConfig())
        ->addListener('127.0.0.1', $port)
        ->setReadTimeout(10)
        ->setWriteTimeout(10)
);

$server->addHttpHandler(function ($req, $res) use ($server) {
    echo "before stream: ", $res->isWritable() ? 'yes' : 'no', "\n";

    $res->sseStart();
    echo "after commit: ", $res->isWritable() ? 'yes' : 'no', "\n";

    $payload = str_repeat('x', 4096);

    try {
        for ($i = 0; $i < 100000; $i++) {
            $res->sseEvent($payload, id: (string) $i);
        }
        echo "peer never left\n";
    } catch (HttpException $e) {
        echo "caught ", $e->getCode(), "\n";
    }

    echo "after peer left: ", $res->isWritable() ? 'yes' : 'no', "\n";

    $server->stop();
});

spawn(function () use ($port) {
    usleep(30000);
    $sock = socket_create(AF_INET, SOCK_STREAM, SOL_TCP);
    socket_connect($sock, '127.0.0.1', $port);
    socket_write($sock, "GET /events HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
    socket_read($sock, 256);
    socket_set_option($sock, SOL_SOCKET, SO_LINGER, ['l_onoff' => 1, 'l_linger' => 0]);
    socket_close($sock);
});

$server->start();
?>
--EXPECT--
before stream: yes
after commit: yes
caught 499
after peer left: no
