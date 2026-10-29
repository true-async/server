--TEST--
HttpServer pool: a bailout from a worker's start() wait completes the clone teardown
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
    ->setWorkers(2)->setShutdownTimeout(2);
$server = new HttpServer($config);
$server->addHttpHandler(function ($req, $res) { $res->setBody('ok'); });
_http_fault_enable('server/pool/worker_wait_bailout', true);
// stop() once the worker has bailed out: a stop that came first would end
// every worker before it reached the fault point.
spawn(function () use ($server) {
    for ($i = 0; $i < 500 && _http_fault_hits('server/pool/worker_wait_bailout') === 0; $i++) {
        delay(10);
    }
    $server->stop();
});
$result = $server->start();
echo 'start=', $result ? 'true' : 'false', "\n";
echo 'start bailout=', _http_fault_hits('server/pool/worker_wait_bailout'), "\n";
_http_fault_disable('server/pool/worker_wait_bailout');
echo "worker cleanup completed\n";
?>
--EXPECTF--
%Astart=false
start bailout=1
worker cleanup completed
