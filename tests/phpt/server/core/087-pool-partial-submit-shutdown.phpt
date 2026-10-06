--TEST--
HttpServer (#351): partial worker submission stops and joins the submitted tasks
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
require __DIR__ . '/../_free_port.inc';
$config = (new HttpServerConfig())->addListener('127.0.0.1', tas_free_port())
    ->setWorkers(3)->setShutdownTimeout(0);
$server = new HttpServer($config);
$server->addHttpHandler(function ($req, $res) { $res->setBody('ok'); });
_http_fault_enable('server/pool/partial_submit', true);
$result = $server->start();
echo 'start=', $result ? 'true' : 'false', "\n";
echo 'partial fault=', _http_fault_hits('server/pool/partial_submit'), "\n";
_http_fault_disable('server/pool/partial_submit');
echo "shutdown joined\n";
?>
--EXPECTF--
%Astart=false
partial fault=1
shutdown joined
