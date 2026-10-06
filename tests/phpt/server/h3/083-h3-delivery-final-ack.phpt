--TEST--
HTTP/3 pool (#351): enqueue/apply do not count a response before its final ACK
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
$tmp = __DIR__ . '/tmp-083-' . getmypid();
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
    ->setWorkers(2)->setStatsEnabled(true);
$server = new HttpServer($config);
$server->addHttpHandler(function ($req, $res) { $res->setBody('ack-body'); });
$client = __DIR__ . '/../../../h3client/h3client';
spawn(function () use ($server, $port, $client, $tmp) {
    delay(600);
    $proc = proc_open('H3CLIENT_DELAY_FINAL_ACK_MS=700 H3CLIENT_DEADLINE_MS=3000 '
        . escapeshellarg($client) . ' 127.0.0.1 ' . $port . ' / GET',
        [0 => ['file', '/dev/null', 'r'], 1 => ['file', "$tmp/body", 'w'],
         2 => ['file', "$tmp/stderr", 'w']], $pipes);
    for ($i = 0; $i < 500; $i++) {
        if (str_contains((string) @file_get_contents("$tmp/stderr"), 'ACK_WAITING')) break;
        delay(5);
    }
    $t = $server->getStats()['totals'];
    echo 'ACK delayed: ', str_contains((string) @file_get_contents("$tmp/stderr"), 'ACK_WAITING')
        ? 'yes' : 'FAIL', "\n";
    echo 'before ACK: completed=', $t['total_requests'], ' 2xx=', $t['responses_2xx_total'], "\n";
    proc_close($proc);
    for ($i = 0; $i < 100; $i++) {
        $t = $server->getStats()['totals'];
        if ($t['total_requests'] === 1) break;
        delay(10);
    }
    echo 'after ACK: completed=', $t['total_requests'], ' 2xx=', $t['responses_2xx_total'],
         ' undelivered=', $t['responses_undelivered_total'], "\n";
    echo 'body: ', file_get_contents("$tmp/body") === 'ack-body' ? 'intact' : 'FAIL', "\n";
    $server->stop();
});
$server->start();
?>
--EXPECTF--
%AACK delayed: yes
before ACK: completed=0 2xx=0
after ACK: completed=1 2xx=1 undelivered=0
body: intact
%A
