--TEST--
HTTP/1 — a terminator that never left retires the connection
--EXTENSIONS--
true_async_server
true_async
sockets
--FILE--
<?php
/* Chunked framing ends at `0\r\n\r\n` and nowhere else, so a body whose
 * terminator did not reach the wire has no end a peer can find (RFC 9112 §7.1).
 * `h1_stream_mark_ended` refuses to seal a stream that already failed, but the
 * terminator's own write was submitted with its answer discarded — and the
 * awaited write path latches nothing on the connection, so every later reader
 * saw a healthy one. The request pipelined behind the broken body was then
 * answered into it: a whole `HTTP/1.1 200 OK` where the peer was counting down
 * an unfinished chunk, which is the desynchronisation the terminator exists to
 * prevent.
 *
 * Reaching it needs the write to be parked when it fails, not merely to fail.
 * The client clamps its receive buffer and never reads, the handler fills the
 * outbound queue past `setStreamWriteBufferBytes` until `tryWrite()` refuses,
 * and a coroutine cancels the handler while `end()` is parked inside the
 * terminator's write.
 *
 * The assertion is server-side: nothing has to be drained, and what it reads is
 * whether the second request was dispatched at all. The settle before `stop()`
 * lets the reactor retire the connection before the loop is torn down. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\delay;
use function Async\current_coroutine;

require_once __DIR__ . '/../_free_port.inc';

$port = tas_free_port();
$server = new HttpServer(
    (new HttpServerConfig())
        ->addListener('127.0.0.1', $port)
        ->setReadTimeout(10)
        ->setWriteTimeout(10)
        ->setStreamWriteBufferBytes(4194304)
);

$seen = [];

$server->addHttpHandler(function ($req, $res) use (&$seen) {
    $seen[] = $req->getUri();

    if ($req->getUri() === '/next') {
        $res->end('second');
        return;
    }

    $res->setStatusCode(200)->setHeader('Content-Type', 'application/octet-stream');
    $res->setNoCompression();

    $co = current_coroutine();
    spawn(function () use ($co) { delay(300); $co->cancel(); });

    $chunk = str_repeat('x', 65536);
    while ($res->tryWrite($chunk)) { /* until the high-water mark refuses */ }

    try {
        $res->end();
        $seen[] = 'end=ok';
    } catch (\Throwable $e) {
        $seen[] = 'end=' . get_class($e);
    }
});

spawn(function () use ($port, $server) {
    usleep(50000);

    $fp   = stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 5);
    $sock = socket_import_stream($fp);
    socket_set_option($sock, SOL_SOCKET, SO_RCVBUF, 4096);

    fwrite($fp,
        "GET /stream HTTP/1.1\r\nHost: x\r\n\r\n" .
        "GET /next HTTP/1.1\r\nHost: x\r\n\r\n");

    delay(1500);            /* the handler parks, the cancel lands, dispose runs */
    fclose($fp);
    delay(400);

    $server->stop();
});

$server->start();
echo "handled: ", implode(',', $seen), "\n";
?>
--EXPECT--
handled: /stream,end=Async\AsyncCancellation
