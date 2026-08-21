--TEST--
HttpResponse — what declares a length, what does not, and what a bad declaration costs
--EXTENSIONS--
true_async_server
true_async
sockets
--FILE--
<?php
/* The audit is a property of the streamed response, and this walks its edges
 * from PHP rather than from the wire.
 *
 * A declaration is taken once, at the first write(), because that is the last
 * moment the handler could still change it — a Content-Length set afterwards
 * is refused by the same guard that refuses every header after the commit.
 *
 * A value that is not a byte count is refused there and then, while the
 * response is still uncommitted and the failure can still become a status. The
 * alternative is to drop it, and a framing header dropped in silence is how a
 * truncated body came to read as complete in the first place.
 *
 * HEAD declares nothing: its write() drops the chunk before the guard, so the
 * length stays on the buffered path where it describes the body a GET would
 * have returned. SSE declares nothing either — it commits its stream through
 * its own path and keeps the framing it has. */

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
    switch ($req->getPath()) {
        case '/bad':
            $res->setHeader('Content-Length', 'twelve');

            try {
                $res->write('x');
            } catch (\Throwable $e) {
                echo "bad: ", get_class($e), ": ", $e->getMessage(), "\n";
            }

            echo "bad committed: ", (int) $res->isHeadersSent(), "\n";
            $res->setStatusCode(500)->setBody("declared badly\n")->end();
            return;

        case '/twice':
            $res->addHeader('Content-Length', '5');
            $res->addHeader('Content-Length', '6');

            try {
                $res->write('x');
            } catch (\Throwable $e) {
                echo "twice: ", $e->getMessage(), "\n";
            }

            $res->setStatusCode(500)->setBody("declared twice\n")->end();
            return;

        case '/late':
            $res->setHeader('Content-Length', '5');
            $res->write('alpha');

            try {
                $res->setHeader('Content-Length', '99');
            } catch (\Throwable $e) {
                echo "late: ", get_class($e), "\n";
            }

            $res->end();
            return;

        case '/frame':
            /* A gRPC frame is body bytes of this response like any other, so a
             * declared length counts it. Without that the frame goes out past
             * the count and the peer reads the surplus as the next response. */
            $res->setHeader('Content-Length', '10');
            $res->write('abc');

            try {
                $res->writeMessage(str_repeat('x', 20));
            } catch (\Throwable $e) {
                echo "frame: ", $e->getMessage(), "\n";
            }

            $res->end('defghij');
            return;

        case '/buffered':
            /* A buffered body never declares — the server states the count it
             * is sending. A handler value that disagreed used to reach the
             * wire, and the peer read the surplus as the next status line. */
            $res->setHeader('Content-Length', '5')->setBody('hello world')->end();
            return;

        case '/exact':
            $res->setHeader('Content-Length', '5');
            $res->write('al');
            $res->write('pha');
            $res->end();
            echo "exact ended: ", (int) $res->isEnded(), "\n";
            return;
    }

    $res->setStatusCode(404)->end();
});

spawn(function () use ($port, $server) {
    usleep(50000);

    $get = static function (string $path) use ($port) {
        $fp = stream_socket_client("tcp://127.0.0.1:$port", $e, $s, 5);
        stream_set_timeout($fp, 3);
        fwrite($fp, "GET $path HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
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

        return [substr($raw, 0, (int) $head_end),
                $head_end === false ? '' : substr($raw, $head_end + 4)];
    };

    [$head, $body] = $get('/bad');
    echo "bad status: ", strtok($head, "\r\n"), "\n";
    echo "bad body: ", json_encode($body), "\n";

    [, $body] = $get('/twice');
    echo "twice body: ", json_encode($body), "\n";

    [$head, $body] = $get('/late');
    echo "late length: ", preg_match('/^content-length:\s*(\d+)/mi', $head, $m) ? $m[1] : '<absent>', "\n";
    echo "late body: ", json_encode($body), "\n";

    [$head, $body] = $get('/frame');
    echo "frame length: ", preg_match('/^content-length:\s*(\d+)/mi', $head, $m) ? $m[1] : '<absent>', "\n";
    echo "frame body: ", json_encode($body), "\n";

    [$head, $body] = $get('/buffered');
    echo "buffered length: ", preg_match('/^content-length:\s*(\d+)/mi', $head, $m) ? $m[1] : '<absent>', "\n";
    echo "buffered body: ", json_encode($body), "\n";

    [$head, $body] = $get('/exact');
    echo "exact length: ", preg_match('/^content-length:\s*(\d+)/mi', $head, $m) ? $m[1] : '<absent>', "\n";
    echo "exact body: ", json_encode($body), "\n";

    $server->stop();
});

$server->start();
?>
--EXPECT--
bad: TrueAsync\HttpServerRuntimeException: write(): Content-Length must be a decimal byte count, got "twelve"
bad committed: 0
bad status: HTTP/1.1 500 Internal Server Error
bad body: "declared badly\n"
twice: write(): Content-Length was declared more than once — a body is framed by one length or by none
twice body: "declared twice\n"
late: TrueAsync\HttpServerRuntimeException
late length: 5
late body: "alpha"
frame: writeMessage(): body would pass the declared Content-Length of 10 bytes — 3 written, 25 offered
frame length: 10
frame body: "abcdefghij"
buffered length: 11
buffered body: "hello world"
exact ended: 1
exact length: 5
exact body: "alpha"
