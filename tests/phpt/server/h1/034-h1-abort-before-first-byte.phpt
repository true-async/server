--TEST--
A stream started but never written is finished, not failed
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
/* sseStart() commits the response object without committing the stream: the
 * status line is decided but no byte of it has left, and every transport keeps
 * a lazy commit for exactly that case. A handler that throws there has
 * produced no half body, so there is nothing to disown — failing the stream
 * would replace an empty but valid answer with a connection that merely stops,
 * and the client would have less to go on, not more.
 *
 * The line that matters is `terminator: 1`: the empty body is finished
 * cleanly. `body: ""` is what separates this from the case where a chunk did
 * reach the wire — there the terminator is the thing withheld. */

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
    $res->sseStart();

    throw new \RuntimeException('nothing was written before this');
});

spawn(function () use ($port, $server) {
    usleep(50000);

    $fp = stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 5);
    stream_set_timeout($fp, 3);
    fwrite($fp, "GET /events HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");

    $raw = '';

    while (!feof($fp)) {
        $piece = fread($fp, 8192);

        if ($piece === false || $piece === '') {
            break;
        }

        $raw .= $piece;
    }

    fclose($fp);

    $head_end = strpos($raw, "\r\n\r\n");
    $body     = $head_end === false ? '' : substr($raw, $head_end + 4);

    echo "status: ", strtok($raw, "\r\n"), "\n";
    echo "terminator: ", (int) ($body === "0\r\n\r\n"), "\n";
    echo "body: ", json_encode(str_replace("0\r\n\r\n", '', $body)), "\n";

    $server->stop();
});

$server->start();
?>
--EXPECT--
status: HTTP/1.1 200 OK
terminator: 1
body: ""
