--TEST--
HttpResponse — a streaming handler that throws leaves the body unterminated and drops keep-alive
--EXTENSIONS--
true_async_server
true_async
sockets
--FILE--
<?php
/* A chunked body ends with the zero chunk, and that terminator is the only
 * thing that tells the client the body is whole. A handler that throws after
 * committing has produced half a body, so the terminator must not be written
 * and the connection must not be handed on: without the terminator the framing
 * is lost, and the peer would read the next status line as the chunk it was
 * still promised.
 *
 * Three assertions. The frames written before the throw are pinned byte for
 * byte, which is what catches an abort that also discards what did reach the
 * wire. The tail must carry no terminator. And the request is sent without
 * `Connection: close`, so the connection reaching EOF is the server's decision
 * and not the client's. */

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

$server->addHttpHandler(function ($req, $res) {
    $res->setStatusCode(200)->setHeader('Content-Type', 'text/plain');
    $res->write('alpha');
    $res->write('beta');

    throw new \RuntimeException('handler failed mid-body');
});

spawn(function () use ($port, $server) {
    usleep(50000);

    $fp = stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 5);
    stream_set_timeout($fp, 3);
    fwrite($fp, "GET /stream HTTP/1.1\r\nHost: x\r\n\r\n");

    $raw = '';

    while (!feof($fp)) {
        $piece = fread($fp, 8192);

        if ($piece === false || $piece === '') {
            break;
        }

        $raw .= $piece;
    }

    $closed = feof($fp);
    fclose($fp);

    $head_end = strpos($raw, "\r\n\r\n");
    $body     = $head_end === false ? '' : substr($raw, $head_end + 4);

    echo "status: ", strtok($raw, "\r\n"), "\n";
    echo "body: ", json_encode($body), "\n";
    echo "terminator: ", (int) (strpos($body, "0\r\n\r\n") !== false), "\n";
    echo "closed by server: ", (int) $closed, "\n";

    $server->stop();
});

$server->start();
?>
--EXPECT--
status: HTTP/1.1 200 OK
body: "5\r\nalpha\r\n4\r\nbeta\r\n"
terminator: 0
closed by server: 1
