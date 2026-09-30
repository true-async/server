--TEST--
HTTP/1: a refusal and a stop() that come while the read is paused on a full buffer
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
/* The read stops while pipelined requests fill the buffer behind a busy
 * handler (#341). Two exits from that state. A request refused with 413 once
 * the handler is done opens the lingering close, which must read again to
 * drain the body the client is still sending; the client's write completing
 * shows it did. And stop() while the read is stopped must still end start(). */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;
use function Async\delay;

require_once __DIR__ . '/../_free_port.inc';

function serve(int $port): HttpServer
{
    $server = new HttpServer((new HttpServerConfig())
        ->addListener('127.0.0.1', $port)
        ->setReadTimeout(10)
        ->setWriteTimeout(10)
        ->setMaxBodySize(4096)
        ->setShutdownTimeout(1));

    $server->addHttpHandler(function ($req, $res) {
        if ($req->getPath() === '/slow') {
            delay(300);
        } elseif ($req->getPath() === '/stuck') {
            delay(30000);
        }

        $res->setStatusCode(200)->setBody('ok')->end();
    });

    return $server;
}

function pipeline(string $first, int $n): string
{
    $wire = "GET $first HTTP/1.1\r\nHost: t\r\n\r\n";

    for ($i = 0; $i < $n; $i++) {
        $wire .= "GET /r$i HTTP/1.1\r\nHost: t\r\nX-Pad: " . str_repeat('x', 40) . "\r\n\r\n";
    }

    return $wire;
}

/* 413 behind a paused read: 120 GETs fill the buffer, then a body 50 times
 * the limit. */
$port = tas_free_port();
$server = serve($port);
$client = spawn(function () use ($port, $server) {
    $big = str_repeat('b', 200000);
    $wire = pipeline('/slow', 120)
          . "POST /big HTTP/1.1\r\nHost: t\r\nContent-Length: " . strlen($big) . "\r\n\r\n$big";

    $c = stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 2);
    stream_set_timeout($c, 10);
    $writer = spawn(fn() => fwrite($c, $wire));

    $in = '';
    while (!feof($c)) {
        $chunk = fread($c, 65536);
        if ($chunk === false || $chunk === '') { break; }
        $in .= $chunk;
    }

    $written = await($writer);
    fclose($c);

    echo "refusal: ", substr_count($in, "HTTP/1.1 200"), " answered, 413 ",
        str_contains($in, "HTTP/1.1 413") ? 'sent' : 'missing', ", body ",
        $written === strlen($wire) ? 'drained' : "cut at $written of " . strlen($wire), "\n";
    $server->stop();
});
$server->start();
await($client);

/* stop() behind a paused read: the handler would run for 30 s, and the
 * shutdown gives it 1 s before cancelling it. */
$port = tas_free_port();
$server = serve($port);
$client = spawn(function () use ($port, $server) {
    $c = stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 2);
    fwrite($c, pipeline('/stuck', 150));
    delay(200);
    $server->stop();
    fclose($c);
});
$watchdog = spawn(function () use ($server) {
    delay(5000);
    echo "FAIL: start() still running 5 s after stop()\n";
    $server->stop();
});
$server->start();
$watchdog->cancel();
await($client);
echo "stop: start() returned\n";
?>
--EXPECT--
refusal: 121 answered, 413 sent, body drained
stop: start() returned
