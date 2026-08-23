--TEST--
HTTP/1: a peer that shuts its write half down still gets the whole response (#249)
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
/* A half-close says "I have sent everything", not "I have gone": the peer goes
 * on reading. The read EOF it produces used to latch the connection unwritable,
 * so a streaming handler lost everything after the first batch — here, the
 * headers and nothing else. Both pipelined requests are answered because the
 * read buffer already holds the second when the EOF arrives. */

require_once __DIR__ . '/../_free_port.inc';

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

const CHUNK_BYTES = 64 * 1024;
const CHUNK_COUNT = 16;

$port = tas_free_port();

$server = new HttpServer(
    (new HttpServerConfig())->addListener('127.0.0.1', $port)->setReadTimeout(5)
);
$server->addHttpHandler(function ($req, $res) {
    $res->setHeader('content-type', 'text/plain');

    for ($i = 0; $i < CHUNK_COUNT; $i++) {
        $res->write(str_repeat('z', CHUNK_BYTES));
    }

    $res->end();
});

$client = spawn(function () use ($port, $server) {
    usleep(50000);

    $fp = stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 3);
    stream_set_timeout($fp, 5);

    /* Two requests in one write, then the write half goes: the server reads
     * both and an EOF behind them. */
    $req = "GET / HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
    fwrite($fp, $req . $req);
    fflush($fp);
    stream_socket_shutdown($fp, STREAM_SHUT_WR);

    $wire = '';

    while (!feof($fp)) {
        $part = fread($fp, 65536);

        if ($part === false || $part === '') { break; }

        $wire .= $part;
    }

    fclose($fp);

    echo 'responses: ', substr_count($wire, "HTTP/1.1 200 OK"), "\n";
    echo 'terminators: ', substr_count($wire, "\r\n0\r\n\r\n"), "\n";
    /* The body is the only 'z' on the wire: neither the status line, nor the
     * headers the server sends, nor the hex chunk sizes carry one. */
    echo 'body bytes: ', substr_count($wire, 'z'), ' of ', CHUNK_BYTES * CHUNK_COUNT * 2, "\n";

    $server->stop();
});

$server->start();
await($client);
echo "done\n";
?>
--EXPECT--
responses: 2
terminators: 2
body bytes: 2097152 of 2097152
done
