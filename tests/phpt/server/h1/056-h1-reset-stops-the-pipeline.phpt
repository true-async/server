--TEST--
HTTP/1 — a peer that reset the connection gets no handler run for what it pipelined
--EXTENSIONS--
true_async_server
true_async
sockets
--FILE--
<?php
/* `h1_stream_mark_ended` refuses to write a terminator onto a connection whose
 * write side has already failed, and drops keep-alive for it. It did not mark
 * the stream dead, and that is the flag `http_request_finalize` reads as "no
 * boundary the peer can find, write nothing more" — so the request pipelined
 * behind the unterminated body was still dispatched. The peer is gone, so no
 * byte of that second answer can reach it; what runs is the handler, with
 * whatever it writes to a database on the way.
 *
 * The reset is what makes it deterministic: SO_LINGER 0 turns the close into an
 * RST, the read completion latches the failure, and the handler is still parked
 * in its delay when `end()` runs into the guard. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\delay;

require_once __DIR__ . '/../_free_port.inc';

$port = tas_free_port();
$server = new HttpServer(
    (new HttpServerConfig())
        ->addListener('127.0.0.1', $port)
        ->setReadTimeout(10)
        ->setWriteTimeout(10)
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
    $res->write('alpha');
    delay(600);              /* the reset lands while the handler is here */
    $res->end();
});

spawn(function () use ($port, $server) {
    usleep(50000);

    $fp   = stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 5);
    $sock = socket_import_stream($fp);

    fwrite($fp,
        "GET /stream HTTP/1.1\r\nHost: x\r\n\r\n" .
        "GET /next HTTP/1.1\r\nHost: x\r\n\r\n");

    delay(200);
    socket_set_option($sock, SOL_SOCKET, SO_LINGER, ['l_onoff' => 1, 'l_linger' => 0]);
    fclose($fp);

    delay(1500);
    $server->stop();
});

$server->start();
echo "handled: ", implode(',', $seen), "\n";
?>
--EXPECT--
handled: /stream
