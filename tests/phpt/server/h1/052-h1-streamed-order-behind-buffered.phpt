--TEST--
A streamed HTTP/1 response waits for the buffered ones queued ahead of it
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
/* A pipelined client matches responses to requests by order (RFC 9112 §9.3.1).
 * Two buffered responses are needed ahead of the stream: the first is in flight,
 * the second is copied into the coalesce tail, and only the tail can be
 * overtaken — the awaited sender submits at conn->io, so its chunk reached the
 * peer between the two, and the client read it as the end of response 1.
 *
 * The streamed pair is there to show the same holds once the tail is empty:
 * /4 must not start before /3 has finished. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;

require_once __DIR__ . '/../_free_port.inc';

$port = tas_free_port();
$server = new HttpServer(
    (new HttpServerConfig())
        ->addListener('127.0.0.1', $port)
        ->setReadTimeout(5)
        ->setWriteTimeout(5)
);

$server->addHttpHandler(function ($req, $res) {
    $n = (int) ltrim($req->getPath(), '/');

    if ($n >= 3) {
        $res->write("W{$n}a");
        $res->end("W{$n}b");

        return;
    }

    $res->setBody("S$n")->end();
});

spawn(function () use ($port, $server) {
    usleep(50000);

    $c = stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 3);
    stream_set_timeout($c, 3);

    fwrite($c,
        "GET /1 HTTP/1.1\r\nHost: x\r\n\r\n" .
        "GET /2 HTTP/1.1\r\nHost: x\r\n\r\n" .
        "GET /3 HTTP/1.1\r\nHost: x\r\n\r\n" .
        "GET /4 HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");

    $all = '';

    while (!feof($c)) {
        $read = fread($c, 65536);

        if ($read === false || $read === '') {
            break;
        }

        $all .= $read;
    }

    fclose($c);

    preg_match_all('/(S\d|W\d[ab])/', $all, $m);
    echo 'marks: ', implode(',', $m[1]), "\n";

    $server->stop();
});

$server->start();
echo "Done\n";
?>
--EXPECT--
marks: S1,S2,W3a,W3b,W4a,W4b
Done
