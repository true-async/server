--TEST--
HttpResponse: trailer API — setTrailer/setTrailers/getTrailers/resetTrailers (state)
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

// We exercise the trailer API surface state-only (the H1 wire path
// drops trailers silently per the stub doc — no need for an H2 client
// to verify the in-handler getters).

$port = 19250 + getmypid() % 1000;

$server = new HttpServer((new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(5)->setWriteTimeout(5));

$lines = [];
$snap = function (string $tag, $val) use (&$lines) {
    if (is_array($val)) {
        ksort($val);
        $lines[] = "$tag = {" . implode(',', array_map(fn($k,$v) => "$k:$v", array_keys($val), $val)) . "}";
    } else {
        $lines[] = "$tag = " . var_export($val, true);
    }
};

$server->addHttpHandler(function ($req, $res) use ($snap, $server) {
    // Initial: empty
    $snap('initial', $res->getTrailers());

    // Single setTrailer
    $res->setTrailer('grpc-status', '0');
    $snap('one', $res->getTrailers());

    // Bulk setTrailers (associative)
    $res->setTrailers(['grpc-message' => 'OK', 'x-debug' => 'trace1']);
    $snap('bulk', $res->getTrailers());

    // setTrailer overrides existing key
    $res->setTrailer('grpc-status', '1');
    $snap('override', $res->getTrailers());

    // resetTrailers wipes all
    $res->resetTrailers();
    $snap('after_reset', $res->getTrailers());

    // resetTrailers on already-empty is a no-op
    $res->resetTrailers();
    $snap('reset_empty', $res->getTrailers());

    $res->setHeader('Content-Type', 'text/plain')->setBody('done');
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
--EXPECT--
initial = {}
one = {grpc-status:0}
bulk = {grpc-message:OK,grpc-status:0,x-debug:trace1}
override = {grpc-message:OK,grpc-status:1,x-debug:trace1}
after_reset = {}
reset_empty = {}
