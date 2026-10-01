--TEST--
HTTP/1: chunk extensions past 16 KiB in one request are answered 413
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
/* Chunk-extension bytes are not body bytes, so no body limit counts them: a
 * request could make the server read them without end. They are capped per
 * request instead. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\delay;

require_once __DIR__ . '/../_free_port.inc';

$port = tas_free_port();
$server = new HttpServer((new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setMaxBodySize(1 << 20)
    ->setReadTimeout(5)
    ->setWriteTimeout(5));

$server->addHttpHandler(function ($req, $res) {
    $res->setBody('handled ' . strlen($req->getBody()));
});

function post_with_extension(int $port, int $ext_len): string
{
    $c = stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 2);
    fwrite($c, "POST / HTTP/1.1\r\nHost: t\r\nConnection: close\r\n"
        . "Transfer-Encoding: chunked\r\n\r\n"
        . "5;x=" . str_repeat('A', $ext_len) . "\r\nhello\r\n0\r\n\r\n");
    $reply = (string) stream_get_contents($c);
    fclose($c);

    return strtok($reply, "\r\n");
}

spawn(function () use ($server, $port) {
    delay(100);
    echo "1 KiB extension: ", post_with_extension($port, 1024), "\n";
    echo "64 KiB extension: ", post_with_extension($port, 64 * 1024), "\n";
    $server->stop();
});

$server->start();
?>
--EXPECT--
1 KiB extension: HTTP/1.1 200 OK
64 KiB extension: HTTP/1.1 413 Content Too Large
