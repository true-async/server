--TEST--
HttpResponse — a body short of its declared Content-Length is failed at end(), not finished
--EXTENSIONS--
true_async_server
true_async
sockets
--FILE--
<?php
/* A declared length is a promise of a byte count, so a handler that calls
 * end() over a shorter body has not finished it. Finishing cleanly there would
 * hand the connection on with the peer still counting down, and the next
 * response would be read as the rest of this one. The stream is failed
 * instead: HTTP/1 has no field for that, so what it can say is to stop and
 * close, which is exactly what a truncated identity body means.
 *
 * The client is written to read as a client does — take the declared length,
 * then look at what actually arrived — so the assertion is the one a real peer
 * would make: fewer bytes than promised, and EOF where the rest should be. */

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
    $res->setStatusCode(200)
        ->setHeader('Content-Type', 'text/plain')
        ->setHeader('Content-Length', '10');
    $res->write('alpha');
    $res->end();

    echo "ended: ", (int) $res->isEnded(), "\n";
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
    $head     = substr($raw, 0, (int) $head_end);
    $body     = $head_end === false ? '' : substr($raw, $head_end + 4);

    echo "declared: ", preg_match('/^content-length:\s*(\d+)/mi', $head, $m) ? $m[1] : '<absent>', "\n";
    echo "arrived: ", strlen($body), "\n";
    echo "body: ", json_encode($body), "\n";
    echo "closed by server: ", (int) $closed, "\n";

    $server->stop();
});

$server->start();
?>
--EXPECT--
ended: 1
declared: 10
arrived: 5
body: "alpha"
closed by server: 1
