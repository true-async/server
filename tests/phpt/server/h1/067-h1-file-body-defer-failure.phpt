--TEST--
HttpServer: a file body whose deferred start cannot be scheduled is answered, not left hanging (static and sendFile)
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
if (!function_exists('_http_fault_enable')) die('skip needs --enable-fault-injection');
if (!function_exists('proc_open')) die('skip needs proc_open');
?>
--FILE--
<?php
/* The engine defers the body onto the next loop tick; the fault point makes
 * that schedule fail. Nothing is on the wire yet, so the request can still get
 * a whole answer: the static handler serves the file on its synchronous path,
 * sendFile() answers 500. The request pipelined behind it is served next. */

require_once __DIR__ . '/../_free_port.inc';
require_once __DIR__ . '/_pipeline_probe.inc';

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\StaticHandler;
use function Async\spawn;
use function Async\await;

$root = __DIR__ . '/tmp-067';
@mkdir($root, 0700, true);
$file = "$root/big.bin";
file_put_contents($file, str_repeat('ABCDEFGHIJKLMNOP', 2 * 1024 * 1024 / 16));

register_shutdown_function(function () use ($root, $file) {
    @unlink($file); @rmdir($root);
});

$point = 'send_file/defer_schedule';
$port = tas_free_port();
$server = new HttpServer((new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(10)->setWriteTimeout(10));
$server->addStaticHandler(new StaticHandler('/s/', $root));
$server->addHttpHandler(function ($req, $res) use ($file) {
    $req->getPath() === '/f' ? $res->sendFile($file) : $res->setBody('small');
});

$client = spawn(function () use ($server, $port, $point) {
    usleep(100000);

    foreach (['static' => '/s/big.bin', 'sendFile' => '/f'] as $name => $path) {
        _http_fault_enable($point);
        echo "$name: ", h1_pipeline_probe('tcp', $port, $path, '/small');
        echo "$name hits: ", _http_fault_hits($point), "\n";
    }

    $server->stop();
});

spawn(function () use ($server) {
    usleep(15000000);
    if ($server->isRunning()) { $server->stop(); }
});

$server->start();
await($client);
?>
--EXPECTF--
static: status=200 declared=2097152 got=2097152 extra=200 end=closed
static hits: 1
sendFile: status=500 declared=%d got=%d extra=200 end=closed
sendFile hits: 1
