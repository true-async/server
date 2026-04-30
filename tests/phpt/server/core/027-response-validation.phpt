--TEST--
HttpResponse: input validation — setStatusCode/setHeader rejects bad input
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

$port = 19260 + getmypid() % 1000;

$server = new HttpServer((new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(5)->setWriteTimeout(5));

$lines = [];
$capture = function (string $tag, callable $fn) use (&$lines) {
    try {
        $fn();
        $lines[] = "$tag: no exception";
    } catch (\Throwable $e) {
        $lines[] = "$tag: " . get_class($e);
    }
};

$server->addHttpHandler(function ($req, $res) use ($capture, $server) {
    // Status code out of range
    $capture('status_too_low',  fn() => $res->setStatusCode(99));
    $capture('status_too_high', fn() => $res->setStatusCode(600));
    $capture('status_zero',     fn() => $res->setStatusCode(0));
    $capture('status_negative', fn() => $res->setStatusCode(-1));
    // Boundary: 100 and 599 should both be accepted
    $capture('status_100_ok',   fn() => $res->setStatusCode(100));
    $capture('status_599_ok',   fn() => $res->setStatusCode(599));

    // Empty header name
    $capture('empty_name',  fn() => $res->setHeader('', 'v'));
    // Header name with CR/LF (smuggling guard)
    $capture('crlf_name',   fn() => $res->setHeader("X-Bad\r\nInjected", 'v'));
    $capture('crlf_value',  fn() => $res->setHeader('X-OK', "v\r\nInjected: yes"));

    // Reset to a working state and ship
    $res->setStatusCode(200)
        ->setHeader('Content-Type', 'text/plain')
        ->setBody('valid');
    $res->end();
    $server->stop();
});

$cli = spawn(function () use ($port) {
    usleep(20000);
    $fp = stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 2);
    fwrite($fp, "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
    while (!feof($fp)) fread($fp, 8192);
    fclose($fp);
});

$server->start();
await($cli);

foreach ($lines as $l) echo "$l\n";
--EXPECTF--
status_too_low: %s
status_too_high: %s
status_zero: %s
status_negative: %s
status_100_ok: no exception
status_599_ok: no exception
empty_name: %s
crlf_name: %s
crlf_value: %s
