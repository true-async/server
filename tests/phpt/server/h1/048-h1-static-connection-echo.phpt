--TEST--
A file response tells an HTTP/1.1 client nothing about the connection, and a 1.0 client that it is kept
--EXTENSIONS--
true_async_server
true_async
sockets
--FILE--
<?php
/* Persistence is the default on HTTP/1.1 (RFC 9112 §9.3), so `Connection:
 * keep-alive` states what the peer already assumes, and RFC 9110 §7.6.1 asks a
 * sender not to generate a connection option it does not need. The two handler
 * paths have asked the peer's version before echoing since #197; the file
 * engine stated the field from the connection's verdict alone, on the served
 * file, on the inline error and on the 416.
 *
 * The 1.0 half is the control: the echo is what makes persistence real for a
 * client that would otherwise treat every response as the last (RFC 2068
 * §19.7.1), so it has to survive the change that removes it from 1.1. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;

require_once __DIR__ . '/../_free_port.inc';

$dir = __DIR__ . '/tmp-048';
@mkdir($dir, 0700, true);
$file = "$dir/asset.txt";
file_put_contents($file, 'file body');

register_shutdown_function(function () use ($dir, $file) {
    @unlink($file); @rmdir($dir);
});

$port = tas_free_port();
$server = new HttpServer(
    (new HttpServerConfig())
        ->addListener('127.0.0.1', $port)
        ->setReadTimeout(10)
        ->setWriteTimeout(5)
);

$server->addHttpHandler(function ($req, $res) use ($file) {
    if ($req->getPath() === '/buffered') {
        $res->setStatusCode(200)->setBody('buffered')->end();
        return;
    }

    $res->sendFile($file);
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

    $ask = static function (string $request) use ($port, $read_message) {
        $fp = stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 5);
        stream_set_timeout($fp, 3);
        fwrite($fp, $request);
        [$head, $body] = $read_message($fp);
        fclose($fp);

        return [
            strtok($head, "\r\n"),
            preg_match('/^connection:\s*(\S+)/mi', $head, $m) ? $m[1] : '<absent>',
            $body,
        ];
    };

    foreach ([
        ['file 1.1',   "GET /asset HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n"],
        ['file 1.0',   "GET /asset HTTP/1.0\r\nConnection: keep-alive\r\n\r\n"],
        ['range 1.1',  "GET /asset HTTP/1.1\r\nHost: 127.0.0.1\r\nRange: bytes=99999-\r\n\r\n"],
        ['buffered',   "GET /buffered HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n"],
    ] as [$label, $request]) {
        [$status, $connection, $body] = $ask($request);
        echo "$label: $status | connection: $connection | body: ", json_encode($body), "\n";
    }

    $server->stop();
});

$server->start();
?>
--EXPECT--
file 1.1: HTTP/1.1 200 OK | connection: <absent> | body: "file body"
file 1.0: HTTP/1.0 200 OK | connection: keep-alive | body: "file body"
range 1.1: HTTP/1.1 416 Range Not Satisfiable | connection: <absent> | body: ""
buffered: HTTP/1.1 200 OK | connection: <absent> | body: "buffered"
