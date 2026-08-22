--TEST--
HttpResponse — a Content-Length declared before the first write() frames the HTTP/1 body
--EXTENSIONS--
true_async_server
true_async
sockets
--FILE--
<?php
/* A streamed body is chunked because its length is unknown. When the handler
 * states it up front the length is known, so the header goes to the client and
 * the body is framed by it: no Transfer-Encoding, no size lines, no terminator.
 *
 * The bytes are read raw and printed as-is, because the point of the test is
 * what the framing looks like on the wire and not what a client makes of it.
 * The second request on the same connection is what proves the framing is
 * consistent: an off-by-one in the length would make the peer read the second
 * status line from inside the first body.
 *
 * A handler Transfer-Encoding is dropped even here. One body cannot be framed
 * twice (RFC 9112 §6.1), and a response carrying both headers is what one
 * intermediary resolves by the length and the next by the chunks. */

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
        ->setHeader('Transfer-Encoding', 'chunked')
        ->setHeader('Content-Length', '9');
    $res->write('alpha');
    $res->write('beta');
    $res->end();
});

spawn(function () use ($port, $server) {
    usleep(50000);

    $fp = stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 5);
    /* The head is read a byte at a time to stop exactly at the blank line, so
     * the timeout has to cover a slow runner rather than a slow server: the
     * server's own read deadline above is 10 s. */
    stream_set_timeout($fp, 10);

    $read_message = static function ($fp) {
        $head = '';

        while (!str_contains($head, "\r\n\r\n")) {
            $c = fread($fp, 1);

            if ($c === false || $c === '') {
                break;
            }

            $head .= $c;
        }

        $len  = (int) (preg_match('/^content-length:\s*(\d+)/mi', $head, $m) ? $m[1] : 0);
        $body = $len > 0 ? fread($fp, $len) : '';

        return [$head, $body];
    };

    fwrite($fp, "GET /stream HTTP/1.1\r\nHost: x\r\n\r\n");
    [$head, $body] = $read_message($fp);

    echo "status: ", strtok($head, "\r\n"), "\n";
    echo "content-length: ", preg_match('/^content-length:\s*(\d+)/mi', $head, $m) ? $m[1] : '<absent>', "\n";
    echo "transfer-encoding: ", preg_match('/^transfer-encoding:/mi', $head) ? 'present' : '<absent>', "\n";
    echo "body: ", json_encode($body), "\n";

    /* Second request on the same connection: the peer's read cursor is where
     * the declared length said it would be, or this answer is unreadable. */
    fwrite($fp, "GET /again HTTP/1.1\r\nHost: x\r\n\r\n");
    [$head2, $body2] = $read_message($fp);

    echo "second status: ", strtok($head2, "\r\n"), "\n";
    echo "second body: ", json_encode($body2), "\n";

    fclose($fp);
    $server->stop();
});

$server->start();
?>
--EXPECT--
status: HTTP/1.1 200 OK
content-length: 9
transfer-encoding: <absent>
body: "alphabeta"
second status: HTTP/1.1 200 OK
second body: "alphabeta"
