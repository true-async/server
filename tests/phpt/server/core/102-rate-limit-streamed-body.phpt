--TEST--
Rate limit (#414): with body streaming on, a refused chunked upload never starts a handler mid-body, and the request behind it is served
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
/* With setBodyStreamingEnabled(true) an admitted chunked upload starts its
 * handler when the headers end and reads the body as it arrives. A refused
 * one must not: its body is dropped while it arrives, the 429 goes out after
 * the last chunk, and the GET pipelined behind it is answered on the same
 * connection. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\RateLimit;
use function Async\spawn;
use function Async\delay;

require_once __DIR__ . '/../_free_port.inc';
require_once __DIR__ . '/_rate_limit.inc';

$port = tas_free_port();
$server = new HttpServer((new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setBodyStreamingEnabled(true)
    ->setReadTimeout(10)
    ->setWriteTimeout(10));

$server->addRateLimit((new RateLimit(1, 60))->methods('POST'));

$started = [];
$server->addHttpHandler(function ($req, $res) use (&$started) {
    $started[] = $req->getMethod() . ' ' . $req->getPath();
    $len = 0;

    if ($req->getMethod() === 'POST') {
        while (($chunk = $req->readBody()) !== null) {
            $len += strlen($chunk);
        }
    }

    $res->setBody("read $len");
});

function chunked_upload($fp, string $path): void
{
    fwrite($fp, "POST $path HTTP/1.1\r\nHost: t\r\nTransfer-Encoding: chunked\r\n\r\n"
              . "4\r\nabcd\r\n");
    delay(200);
}

spawn(function () use ($server, $port, &$started) {
    delay(50);
    $fp = rl_connect($port);

    chunked_upload($fp, '/admitted');
    echo "admitted, handlers started mid-body: ", implode(', ', $started), "\n";
    fwrite($fp, "4\r\nefgh\r\n0\r\n\r\n");
    echo "admitted: ", rl_describe(rl_read_response($fp)), "\n";

    chunked_upload($fp, '/refused');
    echo "refused, handlers started mid-body: ", implode(', ', $started), "\n";
    fwrite($fp, "4\r\nefgh\r\n0\r\n\r\nGET /next HTTP/1.1\r\nHost: t\r\n\r\n");
    echo "refused: ", rl_describe(rl_read_response($fp)), "\n";
    echo "next: ", rl_describe(rl_read_response($fp)), "\n";
    fclose($fp);

    echo "handlers started: ", implode(', ', $started), "\n";
    $server->stop();
});

$server->start();
echo "done\n";
--EXPECT--
admitted, handlers started mid-body: POST /admitted
admitted: 200 body=read 8
refused, handlers started mid-body: POST /admitted
refused: 429 retry-after=60 body=Too Many Requests
next: 200 body=read 0
handlers started: POST /admitted, GET /next
done
