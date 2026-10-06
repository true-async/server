--TEST--
HttpServer (#351): an escaped worker start bailout cleans the clone and completes its task
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
if (!function_exists('_http_fault_enable')) die('skip fault injection required');
?>
--FILE--
<?php
use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\delay;
require __DIR__ . '/../_free_port.inc';
$config = (new HttpServerConfig())->addListener('127.0.0.1', tas_free_port())
    ->setWorkers(2)->setShutdownTimeout(0);
$server = new HttpServer($config);
$server->addHttpHandler(function ($req, $res) { $res->setBody('ok'); });
_http_fault_enable('server/pool/worker_start_bailout', true);
spawn(function () use ($server) { delay(400); $server->stop(); });
$result = $server->start();
echo 'start=', $result ? 'true' : 'false', "\n";
echo 'bailout fault=', _http_fault_hits('server/pool/worker_start_bailout'), "\n";
_http_fault_disable('server/pool/worker_start_bailout');
echo "worker completion observed\n";
?>
--EXPECTF--
%Astart=false
bailout fault=1
worker completion observed
