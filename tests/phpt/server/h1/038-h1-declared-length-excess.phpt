--TEST--
HttpResponse — a write past the declared Content-Length throws and puts nothing on the wire
--EXTENSIONS--
true_async_server
true_async
sockets
--FILE--
<?php
/* Once a length is declared the server holds the body to it, so the write that
 * would pass it is refused at the call rather than after the bytes are gone.
 * The handler can still finish the body it promised: the refused chunk was
 * never queued, so the five bytes it had already written are still exactly the
 * five it declared, and end() completes the response normally.
 *
 * Two things are pinned. The exception carries the numbers a handler needs to
 * see its own arithmetic — declared, written, offered. And the wire shows the
 * refused chunk left no trace: the body is the declared length and the
 * connection is reusable, which it would not be if a byte of "surplus" had
 * escaped. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;

require_once __DIR__ . '/../_free_port.inc';
require_once __DIR__ . '/../_read_exact.inc';

$port = tas_free_port();
$server = new HttpServer(
    (new HttpServerConfig())
        ->addListener('127.0.0.1', $port)
        ->setReadTimeout(10)
        ->setWriteTimeout(5)
);

$server->addHttpHandler(function ($req, $res) {
    /* The reuse probe below runs this handler a second time; one report of the
     * refusal is the subject here. */
    static $reported = false;

    $res->setStatusCode(200)
        ->setHeader('Content-Type', 'text/plain')
        ->setHeader('Content-Length', '5');
    $res->write('alpha');

    try {
        $res->write('surplus');

        if (!$reported) {
            echo "no throw\n";
        }
    } catch (\Throwable $e) {
        if (!$reported) {
            echo "class: ", get_class($e), "\n";
            echo "message: ", $e->getMessage(), "\n";
        }
    }

    $reported = true;
    $res->end();
});

spawn(function () use ($port, $server) {
    usleep(50000);

    $fp = stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 5);
    stream_set_timeout($fp, 3);
    fwrite($fp, "GET /stream HTTP/1.1\r\nHost: x\r\n\r\n");

    $head = '';

    while (!str_contains($head, "\r\n\r\n")) {
        $c = fread($fp, 1);

        if ($c === false || $c === '') {
            break;
        }

        $head .= $c;
    }

    $len  = preg_match('/^content-length:\s*(\d+)/mi', $head, $m) ? (int) $m[1] : 0;
    $body = $len > 0 ? tas_read_exact($fp, $len) : '';

    echo "content-length: ", $len, "\n";
    echo "body: ", json_encode($body), "\n";

    fwrite($fp, "GET /again HTTP/1.1\r\nHost: x\r\n\r\n");
    $tail = fread($fp, 32);
    echo "connection reusable: ", (int) str_starts_with((string) $tail, 'HTTP/1.1 200'), "\n";

    fclose($fp);
    $server->stop();
});

$server->start();
?>
--EXPECT--
class: TrueAsync\HttpServerRuntimeException
message: write(): body would pass the declared Content-Length of 5 bytes — 5 written, 7 offered
content-length: 5
body: "alpha"
connection reusable: 1
