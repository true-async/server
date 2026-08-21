--TEST--
An HTTP/1.0 connection the server keeps is told so, and a declared length keeps it
--EXTENSIONS--
true_async_server
true_async
sockets
--FILE--
<?php
/* An HTTP/1.0 peer treats every response as the last one on the connection
 * unless the server confirms otherwise (RFC 2068 §19.7.1). Without that
 * confirmation a server that holds the socket open is holding it against a
 * client that has already stopped reading, so the header is what makes
 * persistence real rather than one-sided.
 *
 * A declared Content-Length is what leaves the connection reusable in the first
 * place: it gives the peer the boundary a close-delimited body does not have.
 * The second request on the same socket is the proof — with the wrong framing
 * or an off-by-one length it would be read from inside the first body.
 *
 * The buffered response is the second half: it takes a different formatter and
 * had the same silence. */

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
    if ($req->getPath() === '/buffered') {
        $res->setStatusCode(200)->setBody('buffered')->end();
        return;
    }

    $res->setStatusCode(200)
        ->setHeader('Content-Type', 'text/plain')
        ->setHeader('Content-Length', '9');
    $res->write('alpha');
    $res->write('beta');
    $res->end();
});

spawn(function () use ($port, $server) {
    usleep(50000);

    $read_message = static function ($fp) {
        $head = '';

        while (!str_contains($head, "\r\n\r\n")) {
            $c = fread($fp, 1);

            if ($c === false || $c === '') {
                break;
            }

            $head .= $c;
        }

        $len = (int) (preg_match('/^content-length:\s*(\d+)/mi', $head, $m) ? $m[1] : 0);

        return [$head, $len > 0 ? fread($fp, $len) : ''];
    };

    $fp = stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 5);
    stream_set_timeout($fp, 3);

    fwrite($fp, "GET /stream HTTP/1.0\r\nConnection: keep-alive\r\n\r\n");
    [$head, $body] = $read_message($fp);

    echo "status: ", strtok($head, "\r\n"), "\n";
    echo "content-length: ", preg_match('/^content-length:\s*(\d+)/mi', $head, $m) ? $m[1] : '<absent>', "\n";
    echo "transfer-encoding: ", preg_match('/^transfer-encoding:/mi', $head) ? 'present' : '<absent>', "\n";
    echo "connection: ", preg_match('/^connection:\s*(\S+)/mi', $head, $m) ? $m[1] : '<absent>', "\n";
    echo "body: ", json_encode($body), "\n";

    fwrite($fp, "GET /buffered HTTP/1.0\r\nConnection: keep-alive\r\n\r\n");
    [$head, $body] = $read_message($fp);

    echo "second status: ", strtok($head, "\r\n"), "\n";
    echo "second connection: ", preg_match('/^connection:\s*(\S+)/mi', $head, $m) ? $m[1] : '<absent>', "\n";
    echo "second body: ", json_encode($body), "\n";

    fclose($fp);
    $server->stop();
});

$server->start();
?>
--EXPECT--
status: HTTP/1.0 200 OK
content-length: 9
transfer-encoding: <absent>
connection: keep-alive
body: "alphabeta"
second status: HTTP/1.0 200 OK
second connection: keep-alive
second body: "buffered"
