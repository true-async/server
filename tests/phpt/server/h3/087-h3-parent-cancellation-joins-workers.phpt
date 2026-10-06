--TEST--
HTTP/3 pool (#351): repeated parent cancellation joins workers before transport teardown
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
require __DIR__ . '/_h3_skipif.inc';
h3_skipif(['openssl_cli' => true, 'h3client' => true]);
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
$tmp = __DIR__ . '/tmp-087-' . getmypid();
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
$server->addHttpHandler(function ($req, $res) use ($tmp) {
    file_put_contents("$tmp/busy", '1');
    delay(650);
    $res->setBody('finished');
    file_put_contents("$tmp/handler-done", '1');
});
$ended = false;
$parent = spawn(function () use ($server, $tmp, &$ended) {
    $cancelled = false;
    try { $server->start(); }
    catch (Async\AsyncCancellation $e) { $cancelled = true; }
    echo 'cancellation propagated: ', $cancelled ? 'yes' : 'FAIL', "\n";
    echo 'parent waited: ', file_exists("$tmp/handler-done") ? 'yes' : 'FAIL', "\n";
    $ended = true;
});
$client = __DIR__ . '/../../../h3client/h3client';
spawn(function () use ($parent, $port, $client, $tmp, &$ended) {
    delay(600);
    exec('H3CLIENT_DEADLINE_MS=3000 ' . escapeshellarg($client)
         . ' 127.0.0.1 ' . $port . ' / GET > ' . escapeshellarg("$tmp/client") . ' 2>&1 &');
    for ($i = 0; $i < 200 && !file_exists("$tmp/busy"); $i++) delay(10);
    if (!file_exists("$tmp/busy")) { echo "no request\n"; $parent->cancel(); return; }
    $parent->cancel();
    delay(80);
    echo 'first cancellation: ', !$ended ? 'still joining' : 'FAIL', "\n";
    $parent->cancel();
    delay(80);
    echo 'second cancellation: ', !$ended ? 'still joining' : 'FAIL', "\n";
    for ($i = 0; $i < 300 && !$ended; $i++) delay(10);
    echo 'parent completed: ', $ended ? 'yes' : 'FAIL', "\n";
});
?>
--EXPECTF--
%Afirst cancellation: still joining
second cancellation: still joining
%Acancellation propagated: yes
parent waited: yes
parent completed: yes
