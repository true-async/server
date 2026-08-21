--TEST--
A cancelled streaming handler still ends its body cleanly
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
/* An exception and a cancellation reach the dispose path the same way — as a
 * live exception on the coroutine — and the two must not be answered the same
 * way. A handler that threw produced half of what it meant to; a handler the
 * server cancelled at shutdown produced every byte it promised, and there is
 * nothing to disown. Failing that stream would turn every graceful restart
 * into a truncated transfer for every client mid-feed, which is a worse
 * outcome than the one the abort exists to prevent.
 *
 * The frame is complete when the cancellation lands, because the handler is
 * parked in a delay rather than inside a write — that is the difference from
 * `030`, where the cancellation cuts a frame in half and the terminator is
 * correctly withheld. `setShutdownTimeout(0)` skips the grace window so
 * stop() cancels at once instead of letting the delay win the race. */

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
        ->setWriteTimeout(5)
        ->setShutdownTimeout(0)
);

$server->addHttpHandler(function ($req, $res) {
    $res->setStatusCode(200)->setHeader('Content-Type', 'text/plain');
    $res->write('alpha');

    delay(30000);        /* cancelled here, with the frame whole */

    $res->end();
});

spawn(function () use ($port, $server) {
    usleep(50000);

    $fp = stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 5);
    stream_set_timeout($fp, 3);
    fwrite($fp, "GET /stream HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");

    /* Let the first frame land, then take the grace window away. */
    delay(200);
    $server->stop();

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
    echo "body: ", json_encode($body), "\n";
    echo "terminator: ", (int) (strpos($body, "0\r\n\r\n") !== false), "\n";
});

$server->start();
?>
--EXPECT--
status: HTTP/1.1 200 OK
body: "5\r\nalpha\r\n0\r\n\r\n"
terminator: 1
