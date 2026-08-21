--TEST--
A request pipelined behind a close-delimited body is not answered on that connection
--EXTENSIONS--
true_async_server
true_async
sockets
--FILE--
<?php
/* A body the close delimits has no boundary before the close, so the peer reads
 * to EOF. A second response written after the first would arrive as the first
 * one's last bytes — the same desynchronisation a chunked body without its
 * terminator produces, and the connection normally drains its read buffer
 * before honouring a close decision.
 *
 * The first request asks for keep-alive, which is what makes the test bite: the
 * connection would otherwise have been closed for the HTTP/1.0 default and the
 * drain skipped for a reason that has nothing to do with the framing. Both
 * requests go out in one write, so the second is provably in the buffer before
 * the first is dispatched, and the handler counts its calls so a silent run
 * cannot pass either. */

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

$seen = [];

$server->addHttpHandler(function ($req, $res) use (&$seen) {
    $seen[] = $req->getUri();
    $res->setStatusCode(200)->setHeader('Content-Type', 'text/plain');
    $res->write('alpha');
    $res->end();
});

spawn(function () use ($port, $server, &$seen) {
    usleep(50000);

    $fp = stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 5);
    stream_set_timeout($fp, 3);
    fwrite($fp,
        "GET /one HTTP/1.0\r\nConnection: keep-alive\r\n\r\n" .
        "GET /two HTTP/1.0\r\nConnection: keep-alive\r\n\r\n");

    $all = '';

    while (!feof($fp)) {
        $chunk = fread($fp, 4096);

        if ($chunk === false || $chunk === '') {
            break;
        }

        $all .= $chunk;
    }

    fclose($fp);

    echo "status lines: ", substr_count($all, "HTTP/1."), "\n";
    echo "connection: ", preg_match('/^connection:\s*(\S+)/mi', $all, $m) ? $m[1] : '<absent>', "\n";
    echo "body: ", json_encode(substr($all, strpos($all, "\r\n\r\n") + 4)), "\n";
    echo "handler saw: ", json_encode($seen), "\n";

    $server->stop();
});

$server->start();
?>
--EXPECT--
status lines: 1
connection: close
body: "alpha"
handler saw: ["\/one"]
