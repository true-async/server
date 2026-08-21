--TEST--
An HTTP/1.0 client gets a body the close delimits, not one framed by chunked coding
--EXTENSIONS--
true_async_server
true_async
sockets
--FILE--
<?php
/* Chunked coding arrived with HTTP/1.1 and a 1.0 client has no decoder for it,
 * so RFC 9112 §6.1 forbids sending it to one — the size lines would be read as
 * body. The framing left for a body whose length is not known in advance is
 * §6.3 rule 7: the bytes as they are, and the connection close as the boundary.
 *
 * That costs the connection, so the response says `Connection: close` and the
 * server closes when the body ends. The client below reads to EOF and gets
 * exactly the nine bytes the handler wrote.
 *
 * The same handler on 1.1 is the control: nothing about the chunked path
 * changes, and the two runs differ only in the request line. */

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
    $res->setStatusCode(200)->setHeader('Content-Type', 'text/plain');
    $res->write('alpha');
    $res->write('beta');
    $res->end();
});

spawn(function () use ($port, $server) {
    usleep(50000);

    /* Read the whole message to EOF: on 1.0 that is the only boundary there is,
     * and on 1.1 it lets the two runs be printed the same way. */
    $fetch = static function ($port, $request) {
        $fp = stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 5);
        stream_set_timeout($fp, 3);
        fwrite($fp, $request);
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

    [$head, $body] = $fetch($port, "GET /stream HTTP/1.0\r\n\r\n");

    echo "1.0 status: ", strtok($head, "\r\n"), "\n";
    echo "1.0 transfer-encoding: ", preg_match('/^transfer-encoding:/mi', $head) ? 'present' : '<absent>', "\n";
    echo "1.0 connection: ", preg_match('/^connection:\s*(\S+)/mi', $head, $m) ? $m[1] : '<absent>', "\n";
    echo "1.0 body: ", json_encode($body), "\n";

    [$head, $body] = $fetch($port, "GET /stream HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");

    echo "1.1 status: ", strtok($head, "\r\n"), "\n";
    echo "1.1 transfer-encoding: ", preg_match('/^transfer-encoding:/mi', $head) ? 'present' : '<absent>', "\n";
    echo "1.1 body: ", json_encode($body), "\n";

    $server->stop();
});

$server->start();
?>
--EXPECT--
1.0 status: HTTP/1.0 200 OK
1.0 transfer-encoding: <absent>
1.0 connection: close
1.0 body: "alphabeta"
1.1 status: HTTP/1.1 200 OK
1.1 transfer-encoding: present
1.1 body: "5\r\nalpha\r\n4\r\nbeta\r\n0\r\n\r\n"
