--TEST--
HttpResponse SSE — a client that disconnects mid-stream surfaces a catchable HttpException (499); the server survives
--EXTENSIONS--
true_async_server
true_async
sockets
--FILE--
<?php
/* Regression: when the SSE peer goes away mid-stream, a write into the dead
 * socket must reach the handler as a CATCHABLE HttpException (499 "stream
 * closed by peer") — the same signal a completion failure already produces —
 * not as an uncaught reactor AsyncException that takes the whole server down.
 *
 * Covers the awaiting send path (http_connection_send_raw): its submit-failure
 * branch (uv_write() fails immediately, e.g. EPIPE) must absorb the reactor
 * exception the way the fire-and-forget writers already do, so the dead stream
 * surfaces upstream as the canonical 499 instead of leaking to the top level. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\HttpException;
use function Async\spawn;
use function Async\await;

require_once __DIR__ . '/../_free_port.inc';

$port = tas_free_port();
$server = new HttpServer(
    (new HttpServerConfig())
        ->addListener('127.0.0.1', $port)
        ->setReadTimeout(10)
        ->setWriteTimeout(10)
);

$server->addHttpHandler(function ($req, $res) use ($server) {
    $res->sseStart();

    // Keep writing into the (soon to be dead) socket. The peer reads a little
    // then closes, so a write past that point fails — and must be catchable.
    $payload = str_repeat('x', 4096);
    $result = 'no-error';
    try {
        for ($i = 0; $i < 100000; $i++) {
            $res->sseEvent($payload, id: (string) $i);
        }
    } catch (HttpException $e) {
        $result = 'caught HttpException ' . $e->getCode();
    } catch (Throwable $e) {
        $result = 'caught ' . get_class($e);
    }

    echo $result, "\n";
    $server->stop();
});

$client = spawn(function () use ($port) {
    usleep(30000);
    $sock = socket_create(AF_INET, SOCK_STREAM, SOL_TCP);
    socket_connect($sock, '127.0.0.1', $port);
    socket_write($sock, "GET /events HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
    socket_read($sock, 256);   // read a little, then ABORT the peer mid-stream

    // SO_LINGER {on, 0} makes close() send a RST instead of a FIN, so a later
    // server write fails at submit (uv_write → EPIPE/ECONNRESET) — the path that
    // used to leak an uncaught AsyncException, not the gracefully-handled FIN one.
    socket_set_option($sock, SOL_SOCKET, SO_LINGER, ['l_onoff' => 1, 'l_linger' => 0]);
    socket_close($sock);
});

$server->start();
await($client);
echo "server survived\n";
?>
--EXPECT--
caught HttpException 499
server survived
