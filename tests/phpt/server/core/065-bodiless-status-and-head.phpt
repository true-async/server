--TEST--
HttpResponse — a message that carries no body does not get one, and says so differently on each path
--EXTENSIONS--
true_async_server
true_async
sockets
--FILE--
<?php
/* RFC 9112 §6.3 rule 1: a 1xx, a 204 and a 304 end at the blank line whatever
 * the header fields say, and so does any response to HEAD. Bytes written past
 * that point are read by the peer as the next message's status line, which is
 * why every request on the connection after such a response is unreadable.
 *
 * The two paths answer differently, and the difference is deliberate. A
 * streaming call refuses: the response is still uncommitted there, so the
 * handler can catch the exception and answer with a status it means. A buffered
 * body is dropped: the status may legitimately have been chosen after the body
 * was built — a conditional GET renders a representation and then finds the
 * ETag matches — and at format time there is no one left to tell.
 *
 * A 304 keeps a handler Content-Length, because there the number describes the
 * representation a 200 would have carried (RFC 9110 §8.6); a 204 loses it,
 * which §8.6 requires. A HEAD keeps the headers a GET would have sent and drops
 * the body through every call that emits one, dialects included. */

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
        case '/buffered':
            /* Over the writev threshold on purpose: a body that size is sent
             * by the two-part formatter, and the guard that drops it there is
             * a second one. Four bytes only ever reach the single-buffer
             * formatter, and left it uncovered. */
            $res->setStatusCode(204)->setBody(str_repeat('a', 2048))->end();
            return;

        case '/message':
            /* The HEAD drop on the gRPC dialect. writeMessage() frames its own
             * bytes, so a message let through here would arrive under a status
             * line that promised none. */
            $res->writeMessage("\x00\x00\x00\x00\x02hi");
            $res->end();
            return;

        case '/streamed':
            $res->setStatusCode(204);

            try {
                $res->write('leak');
            } catch (\Throwable $e) {
                echo "streamed: ", get_class($e), ": ", $e->getMessage(), "\n";
            }

            echo "streamed committed: ", (int) $res->isHeadersSent(), "\n";
            $res->setStatusCode(500)->setBody("refused\n")->end();
            return;

        case '/sse':
            $res->setStatusCode(304);

            try {
                $res->sseStart();
            } catch (\Throwable $e) {
                echo "sse: ", get_class($e), ": ", $e->getMessage(), "\n";
            }

            echo "sse committed: ", (int) $res->isHeadersSent(), "\n";
            $res->setStatusCode(500)->setBody("refused\n")->end();
            return;

        case '/notmodified':
            $res->setStatusCode(304)->setHeader('Content-Length', '1234')->setBody('x')->end();
            return;

        case '/interim':
            $res->setStatusCode(100)->setHeader('Content-Length', '7')->setBody('ignored')->end();
            return;

        case '/handlerte':
            /* One body cannot be framed twice. The server states the count it
             * is sending, so a handler Transfer-Encoding beside it is the pair
             * RFC 9112 §6.1 forbids and §6.3 names as the smuggling shape. */
            $res->setHeader('Transfer-Encoding', 'chunked')->setBody('hello')->end();
            return;

        case '/events':
            $res->sseEvent('hi');
            $res->end();
            return;
    }

    $res->setBody('root')->end();
});

spawn(function () use ($port, $server) {
    usleep(50000);

    /* Read one message: headers, then exactly the declared body if any. A
     * message with no Content-Length ends at the blank line here, which is what
     * the rule says and what the connection's reuse depends on. */
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

    /* Two requests in one write: if the 204 leaked its body, the second status
     * line would be read from inside it. */
    fwrite($fp, "GET /buffered HTTP/1.1\r\nHost: x\r\n\r\nGET / HTTP/1.1\r\nHost: x\r\n\r\n");
    [$head, $body] = $read_message($fp);

    echo "buffered: ", strtok($head, "\r\n"), "\n";
    echo "buffered content-length: ",
        preg_match('/^content-length:/mi', $head) ? 'present' : '<absent>', "\n";
    echo "buffered body: ", json_encode($body), "\n";

    [$head, $body] = $read_message($fp);
    echo "next on the connection: ", strtok($head, "\r\n"), " ", json_encode($body), "\n";

    fwrite($fp, "GET /notmodified HTTP/1.1\r\nHost: x\r\n\r\n");
    $head = '';

    while (!str_contains($head, "\r\n\r\n")) {
        $c = fread($fp, 1);

        if ($c === false || $c === '') {
            break;
        }

        $head .= $c;
    }

    echo "304: ", strtok($head, "\r\n"), "\n";
    echo "304 content-length: ",
        preg_match('/^content-length:\s*(\d+)/mi', $head, $m) ? $m[1] : '<absent>', "\n";

    fwrite($fp, "GET /interim HTTP/1.1\r\nHost: x\r\n\r\n");
    $head = '';

    while (!str_contains($head, "\r\n\r\n")) {
        $c = fread($fp, 1);

        if ($c === false || $c === '') {
            break;
        }

        $head .= $c;
    }

    echo "100: ", strtok($head, "\r\n"), "\n";
    echo "100 content-length: ",
        preg_match('/^content-length:/mi', $head) ? 'present' : '<absent>', "\n";

    fwrite($fp, "GET /handlerte HTTP/1.1\r\nHost: x\r\n\r\n");
    [$head, $body] = $read_message($fp);

    echo "handler TE: ", preg_match('/^transfer-encoding:/mi', $head) ? 'present' : '<absent>', "\n";
    echo "handler TE content-length: ",
        preg_match('/^content-length:\s*(\d+)/mi', $head, $m) ? $m[1] : '<absent>', "\n";
    echo "handler TE body: ", json_encode($body), "\n";

    fwrite($fp, "HEAD /events HTTP/1.1\r\nHost: x\r\n\r\n");
    $head = '';

    while (!str_contains($head, "\r\n\r\n")) {
        $c = fread($fp, 1);

        if ($c === false || $c === '') {
            break;
        }

        $head .= $c;
    }

    echo "HEAD sse: ", strtok($head, "\r\n"), "\n";
    echo "HEAD sse content-type: ",
        preg_match('/^content-type:\s*(\S+)/mi', $head, $m) ? $m[1] : '<absent>', "\n";
    echo "HEAD sse transfer-encoding: ",
        preg_match('/^transfer-encoding:/mi', $head) ? 'present' : '<absent>', "\n";

    /* Pipelined behind the HEAD so the next status line is read from wherever
     * the message really ended: a frame let through would put it inside one. */
    fwrite($fp, "HEAD /message HTTP/1.1\r\nHost: x\r\n\r\nGET / HTTP/1.1\r\nHost: x\r\n\r\n");
    $head = '';

    while (!str_contains($head, "\r\n\r\n")) {
        $c = fread($fp, 1);

        if ($c === false || $c === '') {
            break;
        }

        $head .= $c;
    }

    echo "HEAD message: ", strtok($head, "\r\n"), "\n";
    [$head, $body] = $read_message($fp);
    echo "next after HEAD message: ", strtok($head, "\r\n"), " ", json_encode($body), "\n";

    fwrite($fp, "GET /streamed HTTP/1.1\r\nHost: x\r\n\r\n");
    [$head, $body] = $read_message($fp);
    echo "streamed answer: ", strtok($head, "\r\n"), " ", json_encode($body), "\n";

    fwrite($fp, "GET /sse HTTP/1.1\r\nHost: x\r\n\r\n");
    [$head, $body] = $read_message($fp);
    echo "sse answer: ", strtok($head, "\r\n"), " ", json_encode($body), "\n";

    fclose($fp);
    $server->stop();
});

$server->start();
?>
--EXPECT--
buffered: HTTP/1.1 204 No Content
buffered content-length: <absent>
buffered body: ""
next on the connection: HTTP/1.1 200 OK "root"
304: HTTP/1.1 304 Not Modified
304 content-length: 1234
100: HTTP/1.1 100 Continue
100 content-length: <absent>
handler TE: <absent>
handler TE content-length: 5
handler TE body: "hello"
HEAD sse: HTTP/1.1 200 OK
HEAD sse content-type: text/event-stream
HEAD sse transfer-encoding: <absent>
HEAD message: HTTP/1.1 200 OK
next after HEAD message: HTTP/1.1 200 OK "root"
streamed: TrueAsync\HttpServerRuntimeException: write(): status 204 carries no body — the message ends at the header block
streamed committed: 0
streamed answer: HTTP/1.1 500 Internal Server Error "refused\n"
sse: TrueAsync\HttpServerRuntimeException: Cannot start SSE on status 304 — it carries no body
sse committed: 0
sse answer: HTTP/1.1 500 Internal Server Error "refused\n"
