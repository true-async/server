--TEST--
A pipelined request behind a failed stream is not answered on that connection
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
/* Withholding the terminator is only half the job. The connection normally
 * answers a request already sitting in the read buffer before it honours a
 * close decision, on the reasoning that a client expects a reply to every
 * request it sent — and that reasoning stops holding the moment the framing
 * is gone. The peer is still counting down the bytes an unfinished chunk
 * promised it, so the next response would arrive as that chunk's data: a
 * complete `HTTP/1.1 200 OK` read as body, which is the desynchronisation the
 * missing terminator exists to prevent.
 *
 * Both requests go out in one write, so the second is provably in the buffer
 * before the first is dispatched. The assertion is that it produced no second
 * status line — and, since the handler counts its calls, that the server did
 * not quietly run it either. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;

require_once __DIR__ . '/../_free_port.inc';

$port = tas_free_port();
$server = new HttpServer(
    (new HttpServerConfig())
        ->addListener('127.0.0.1', $port)
        ->setReadTimeout(10)
        ->setWriteTimeout(5)
);

$seen = [];

$server->addHttpHandler(function ($req, $res) use (&$seen) {
    $seen[] = $req->getUri();

    if ($req->getUri() === '/next') {
        $res->end('second');
        return;
    }

    $res->setStatusCode(200)->setHeader('Content-Type', 'text/plain');
    $res->write('half');

    throw new \RuntimeException('handler failed mid-body');
});

spawn(function () use ($port, $server) {
    usleep(50000);

    $fp = stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 5);
    stream_set_timeout($fp, 3);
    fwrite($fp,
        "GET /stream HTTP/1.1\r\nHost: x\r\n\r\n" .
        "GET /next HTTP/1.1\r\nHost: x\r\n\r\n");

    $raw = '';

    while (!feof($fp)) {
        $piece = fread($fp, 8192);

        if ($piece === false || $piece === '') {
            break;
        }

        $raw .= $piece;
    }

    fclose($fp);

    echo "status lines: ", substr_count($raw, "HTTP/1.1 200 OK"), "\n";
    echo "second body: ", (int) (strpos($raw, 'second') !== false), "\n";
    echo "terminator: ", (int) (strpos($raw, "0\r\n\r\n") !== false), "\n";

    $server->stop();
});

$server->start();

echo "handled: ", implode(',', $seen), "\n";
?>
--EXPECT--
status lines: 1
second body: 0
terminator: 0
handled: /stream
