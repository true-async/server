--TEST--
trySseEvent() and tryWriteMessage() — a peer that is gone throws 499 rather than answering false
--EXTENSIONS--
true_async_server
true_async
sockets
--FILE--
<?php
/* False from either twin means one thing: the queue is full. A client that has
 * gone is the other answer, and it arrives as a catchable HttpException 499 —
 * the same signal write() and sseEvent() give. The peer here aborts with a RST
 * (SO_LINGER {1,0}) so the next write fails at submit rather than draining a
 * graceful FIN.
 *
 * The response stays uncommitted on that throw, which is what lets the handler
 * answer with a status afterwards on a response that never started. */

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
    $grpc = $req->getPath() === '/grpc';
    $payload = str_repeat('x', 4096);
    $result = 'no-error';

    try {
        for ($i = 0; $i < 100000; $i++) {
            if ($grpc) {
                $res->tryWriteMessage($payload);
            } else {
                $res->trySseEvent($payload, id: (string) $i);
            }
        }
    } catch (HttpException $e) {
        $result = 'caught HttpException ' . $e->getCode();
    } catch (Throwable $e) {
        $result = 'caught ' . get_class($e);
    }

    echo ($grpc ? 'grpc' : 'sse'), ': ', $result, "\n";

    if ($grpc) {
        $server->stop();
    }
});

$client = spawn(function () use ($port) {
    foreach (['/events', '/grpc'] as $path) {
        usleep(30000);
        $sock = socket_create(AF_INET, SOCK_STREAM, SOL_TCP);
        socket_connect($sock, '127.0.0.1', $port);
        socket_write($sock, "GET $path HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
        socket_read($sock, 256);   // read a little, then abort the peer mid-stream
        socket_set_option($sock, SOL_SOCKET, SO_LINGER, ['l_onoff' => 1, 'l_linger' => 0]);
        socket_close($sock);
    }
});

$server->start();
await($client);
echo "server survived\n";
?>
--EXPECT--
sse: caught HttpException 499
grpc: caught HttpException 499
server survived
