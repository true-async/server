--TEST--
HttpResponse::tryWrite() — HTTP/1 refuses instead of parking once the outbound tail is over the high-water mark
--EXTENSIONS--
true_async_server
true_async
sockets
--FILE--
<?php
/* Before the transport learnt to refuse, HTTP/1 had no sendable op at all:
 * tryWrite() answered true every time and blocked inside the write, so the
 * "refused — do something else" branch of a handler was dead code on the most
 * common protocol.
 *
 * The client here connects, sends the request and then stops reading. The
 * handler offers 64 KiB chunks; once the coalesced tail passes the high-water
 * mark the offer must come back false, and the handler must still be running
 * rather than parked inside uv_write. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;

require_once __DIR__ . '/../_free_port.inc';

$port = tas_free_port();
$server = new HttpServer(
    (new HttpServerConfig())
        ->addListener('127.0.0.1', $port)
        ->setStreamWriteBufferBytes(65536)
        ->setReadTimeout(10)
        ->setWriteTimeout(10)
);

$server->addHttpHandler(function ($req, $res) use ($server) {
    $res->setHeader('Content-Type', 'application/octet-stream');
    $res->setNoCompression();

    $chunk = str_repeat('x', 65536);
    $accepted = 0;
    $refused = 0;

    for ($i = 0; $i < 200; $i++) {
        if ($res->tryWrite($chunk)) {
            $accepted++;
        } else {
            $refused++;
            break;
        }
    }

    echo "accepted>0: ", $accepted > 0 ? 'yes' : 'no', "\n";
    echo "refused: ", $refused > 0 ? 'yes' : 'no', "\n";

    $server->stop();
});

spawn(function () use ($port) {
    usleep(30000);
    $sock = socket_create(AF_INET, SOCK_STREAM, SOL_TCP);
    socket_connect($sock, '127.0.0.1', $port);
    socket_write($sock, "GET /stream HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
    // Deliberately never read: the server's outbound tail grows past the mark.
    usleep(900000);
    socket_close($sock);
});

$server->start();
?>
--EXPECT--
accepted>0: yes
refused: yes
