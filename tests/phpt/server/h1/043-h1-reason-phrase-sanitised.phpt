--TEST--
A CR or LF in a reason phrase does not split the response
--EXTENSIONS--
true_async_server
true_async
sockets
--FILE--
<?php
/* The reason phrase sits between the status code and the CRLF that ends the
 * status line, so a CR or an LF inside it ends that line early and everything
 * behind it is read as header fields — and, past a blank line, as a second
 * response the server never sent.
 *
 * Handler data reaches that field by two routes. An uncaught exception's
 * message becomes the reason phrase of the 500 the dispose path synthesises, so
 * `throw new RuntimeException("bad path: $path")` puts request data on the
 * status line; and setReasonPhrase() takes whatever it is given.
 *
 * RFC 9112 §4 allows HTAB, SP, VCHAR and obs-text there. Everything else
 * becomes a space, which keeps a UTF-8 message readable — obs-text is
 * 0x80..0xFF — while leaving nothing that can end a line. The body is not
 * touched: Content-Length frames it, so it may hold any bytes, and the test
 * reads it back to show the message itself survived intact. */

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

$payload = "boom\r\nX-Injected: yes\r\nContent-Length: 0\r\n\r\nHTTP/1.1 200 OK";

/* The setter's payload carries a NUL as well, and it is not decoration: the
 * status line is built with a strlen-based append, so a NUL reaching it would
 * end the phrase there and leave the rest of the header block off the wire.
 * The sanitiser is what keeps that impossible, and narrowing it to CR and LF
 * alone — the obvious way to make it smaller — would reopen it with nothing
 * failing. The thrown message cannot carry one: it reaches the phrase and the
 * body as a C string and is already cut at the first NUL. */
$setter_payload = "boom\x00" . substr($payload, 4);

$server->addHttpHandler(function ($req, $res) use ($payload, $setter_payload) {
    if ($req->getPath() === '/setter') {
        $res->setReasonPhrase($setter_payload)->setBody('set')->end();
        return;
    }

    throw new \RuntimeException($payload);
});

spawn(function () use ($port, $server) {
    usleep(50000);

    $fetch = static function ($port, $path) {
        $fp = stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 5);
        stream_set_timeout($fp, 3);
        fwrite($fp, "GET $path HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
        $all = '';

        while (!feof($fp)) {
            $chunk = fread($fp, 4096);

            if ($chunk === false || $chunk === '') {
                break;
            }

            $all .= $chunk;
        }

        fclose($fp);
        $split = strpos($all, "\r\n\r\n");

        return [substr($all, 0, $split), substr($all, $split + 4)];
    };

    [$head, $body] = $fetch($port, '/throws');

    echo "status line: ", json_encode(strtok($head, "\r\n")), "\n";
    echo "header lines: ", substr_count($head, "\r\n") + 1, "\n";
    echo "injected header: ", preg_match('/^x-injected:/mi', $head) ? 'present' : '<absent>', "\n";
    echo "second status line in body: ", str_contains($body, 'HTTP/1.1 200 OK') ? 'yes' : 'no', "\n";
    echo "body length: ", strlen($body), "\n";

    [$head, $body] = $fetch($port, '/setter');

    echo "setter status line: ", json_encode(strtok($head, "\r\n")), "\n";
    echo "setter injected header: ", preg_match('/^x-injected:/mi', $head) ? 'present' : '<absent>', "\n";
    echo "setter body: ", json_encode($body), "\n";

    $server->stop();
});

$server->start();
?>
--EXPECT--
status line: "HTTP\/1.1 500 boom  X-Injected: yes  Content-Length: 0    HTTP\/1.1 200 OK"
header lines: 4
injected header: <absent>
second status line in body: yes
body length: 59
setter status line: "HTTP\/1.1 200 boom   X-Injected: yes  Content-Length: 0    HTTP\/1.1 200 OK"
setter injected header: <absent>
setter body: "set"
