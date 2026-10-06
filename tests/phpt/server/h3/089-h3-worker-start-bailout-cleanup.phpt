--TEST--
HTTP/3 pool (#351): a bailout from start after inbox publication completes clone teardown
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
require __DIR__ . '/_h3_skipif.inc';
h3_skipif(['openssl_cli' => true]);
if (!function_exists('_http_fault_enable')) die('skip fault injection required');
?>
--ENV--
TRUE_ASYNC_SERVER_REACTOR_POOL=1
PHP_HTTP3_DISABLE_RETRY=1
--FILE--
<?php
use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\delay;
require __DIR__ . '/_h3_skipif.inc';
require_once __DIR__ . '/../_free_port.inc';
$tmp = __DIR__ . '/tmp-089-' . getmypid();
mkdir($tmp, 0700);
register_shutdown_function(function () use ($tmp) {
    foreach (glob("$tmp/*") as $f) @unlink($f);
    @rmdir($tmp);
});
if (!h3_gen_cert("$tmp/key.pem", "$tmp/cert.pem")) die("cert failed\n");
$port = tas_free_port_span(2);
$config = (new HttpServerConfig())->addListener('127.0.0.1', $port + 1)
    ->addHttp3Listener('127.0.0.1', $port)
    ->setCertificate("$tmp/cert.pem")->setPrivateKey("$tmp/key.pem")
    ->setWorkers(2)->setShutdownTimeout(2);
$server = new HttpServer($config);
$server->addHttpHandler(function ($req, $res) { $res->setBody('ok'); });
_http_fault_enable('server/pool/worker_wait_bailout', true);
spawn(function () use ($server) { delay(400); $server->stop(); });
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
