--TEST--
A parse error answers after the pipelined responses ahead of it, not between them
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
/* The 4xx for a malformed request is built in the read path, which is where the
 * bytes were rejected — ahead of the responses to the valid requests parsed out
 * of the same buffer. It used to reach the socket from there directly, past the
 * write in flight and past the tail behind it, so a peer reading in order
 * (RFC 9112 §9.3.1) took the 400 as the answer to its second request and the
 * second response as the answer to its third.
 *
 * The bodies were never reordered among themselves; the status line order is
 * what the client matches on, and that is what this reads. */

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
    $res->setBody('S' . ltrim($req->getPath(), '/'))->end();
});

spawn(function () use ($port, $server) {
    usleep(50000);

    $c = stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 3);
    stream_set_timeout($c, 3);

    /* Two valid requests and a malformed one, written together so the parser
     * sees all three in one feed. */
    fwrite($c,
        "GET /1 HTTP/1.1\r\nHost: x\r\n\r\n" .
        "GET /2 HTTP/1.1\r\nHost: x\r\n\r\n" .
        "GET /3 HTTP/1.1\r\nHost: x\r\nBad Header\r\n\r\n");

    $all = '';
    while (!feof($c)) {
        $read = fread($c, 65536);

        if ($read === false || $read === '') {
            break;
        }

        $all .= $read;
    }

    fclose($c);

    preg_match_all('/HTTP\/1\.1 (\d{3})/', $all, $m);
    echo 'statuses: ', implode(',', $m[1]), "\n";

    preg_match_all('/\r\n\r\n(S\d)/', $all, $b);
    echo 'bodies: ', implode(',', $b[1]), "\n";

    $server->stop();
});

$server->start();
echo "Done\n";
?>
--EXPECT--
statuses: 200,200,400
bodies: S1,S2
Done
