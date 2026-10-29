--TEST--
HTTP/1: a streamed body the handler does not read is refused past memory_limit, and the worker keeps serving
--EXTENSIONS--
true_async_server
true_async
--INI--
memory_limit=16M
--FILE--
<?php
/* The streaming queue holds each chunk until the handler reads it. A handler
 * that does not read lets the queue grow to setMaxBodySize (128 MiB), past
 * memory_limit (16 MiB) and past the hard limit (32 MiB), where an allocation
 * that is not fallible ends the process with exit code 137. The chunk copy is
 * fallible: the request is refused and the next connection is served. */
use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;
use function Async\delay;

require_once __DIR__ . '/../_free_port.inc';

$port = tas_free_port();
$server = new HttpServer((new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setBodyStreamingEnabled(true)
    ->setMaxBodySize(128 * 1024 * 1024)
    ->setReadTimeout(5)
    ->setWriteTimeout(5));

$server->addHttpHandler(function ($req, $res) {
    if ($req->getPath() === '/upload') {
        delay(3000);
    }

    $res->setStatusCode(200)->setBody('ok')->end();
});

$client = spawn(function () use ($port, $server) {
    usleep(30000);

    $fp = stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 2);
    fwrite($fp, "POST /upload HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n");

    $mib = str_repeat('x', 1 << 20);
    $frame = dechex(strlen($mib)) . "\r\n" . $mib . "\r\n";

    for ($i = 0; $i < 48; $i++) {
        if (@fwrite($fp, $frame) === false) {
            break;
        }
    }

    stream_set_timeout($fp, 5);
    $response = stream_get_contents($fp);
    fclose($fp);
    echo "upload: ", explode("\r\n", $response)[0] ?: '(closed)', "\n";

    $fp2 = stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 2);
    fwrite($fp2, "GET /ping HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
    stream_set_timeout($fp2, 5);
    echo "next: ", explode("\r\n", stream_get_contents($fp2))[0], "\n";
    fclose($fp2);

    $server->stop();
});

$server->start();
await($client);
echo "done\n";
?>
--EXPECTF--
upload: %s
next: HTTP/1.1 200 OK
done
