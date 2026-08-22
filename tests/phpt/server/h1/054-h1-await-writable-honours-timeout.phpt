--TEST--
HttpResponse::awaitWritable($ms) returns on its own bound, not on the write deadline
--EXTENSIONS--
true_async_server
true_async
sockets
--FILE--
<?php
/* The slot's contract is that a nonzero argument is the caller's own bound;
 * zero asks for the transport's write deadline. A transport that reads the
 * argument and then waits out the deadline anyway parks a handler for seconds
 * where it asked for a fraction of one — and on a pool that thread is carrying
 * other requests.
 *
 * The peer here never reads, so the handler first has to fill the socket: while
 * the kernel still takes bytes a refusal is answered in microseconds, and only
 * once nothing moves can the wait reach its bound. The deadline is ten seconds
 * and the bound 200 ms, so an answer between the two tells which was used. */

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
    $ms    = 0.0;

    /* Offer until a wait actually has to wait: that is the socket saying it
     * has stopped taking bytes. */
    for ($i = 0; $i < 400 && $ms < 50; $i++) {
        if ($res->tryWrite($chunk)) {
            continue;
        }

        $t0 = hrtime(true);
        $res->awaitWritable(200);
        $ms = (hrtime(true) - $t0) / 1e6;
    }

    echo 'socket_filled=',       ($ms >= 50 ? 1 : 0), "\n";
    echo 'bounded_by_argument=', ($ms < 1000 ? 1 : 0), "\n";

    $server->stop();
});

spawn(function () use ($port) {
    usleep(30000);
    $sock = socket_create(AF_INET, SOCK_STREAM, SOL_TCP);
    socket_connect($sock, '127.0.0.1', $port);
    socket_write($sock, "GET /stream HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
    /* Never read: the tail stays over the mark. */
    usleep(2000000);
    socket_close($sock);
});

$server->start();
echo "Done\n";
?>
--EXPECT--
socket_filled=1
bounded_by_argument=1
Done
