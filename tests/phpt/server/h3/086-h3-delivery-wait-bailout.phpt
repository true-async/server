--TEST--
HTTP/3 pool (#351): bailout with a linked capacity waiter retracts subscriptions and owned wire
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
require __DIR__ . '/_h3_skipif.inc';
h3_skipif(['openssl_cli' => true, 'h3client' => true]);
if (!function_exists('_http_fault_enable')) die('skip fault injection required');
?>
--ENV--
TRUE_ASYNC_SERVER_REACTOR_POOL=1
PHP_HTTP3_DISABLE_RETRY=1
--FILE--
<?php
use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\LogSeverity;
use function Async\spawn;
use function Async\delay;
require __DIR__ . '/_h3_skipif.inc';
require_once __DIR__ . '/../_free_port.inc';

$tmp = __DIR__ . '/tmp-086-' . getmypid();
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
    ->setWorkers(2)->setStatsEnabled(true)->setWriteTimeout(2)
    ->setReactorMailboxCapacity(64)
    ->setLogSinks([['type' => 'file', 'path' => "$tmp/access.log", 'format' => 'json',
                   'category' => 'access', 'level' => LogSeverity::INFO]]);
$server = new HttpServer($config);
$server->addHttpHandler(function ($req, $res) {
    if ($req->getPath() === '/bailout') {
        _http_fault_enable('h3/reverse/queue_full', false);
        _http_fault_enable('h3/delivery/wait_bailout', true);
        $res->setBody('must-not-leave');
    } else $res->setBody('recovered');
});
$client = __DIR__ . '/../../../h3client/h3client';

spawn(function () use ($server, $port, $client, $tmp) {
    delay(600);
    foreach (['bailout', 'recovery'] as $path) {
        $out = (string) shell_exec('H3CLIENT_DEADLINE_MS=2500 ' . escapeshellarg($client)
            . ' 127.0.0.1 ' . $port . ' /' . $path . ' GET 2>&1');
        _http_fault_disable('h3/reverse/queue_full');
        if ($path === 'bailout') {
            echo 'bailout: ', !str_contains($out, 'STATUS=200')
                && !str_contains($out, 'timeout') ? 'terminated' : 'FAIL', "\n";
        } else echo 'next request: ', str_contains($out, 'STATUS=200')
            && str_contains($out, 'recovered') ? 'intact' : 'FAIL', "\n";
    }
    echo 'linked wait fault: ', _http_fault_hits('h3/delivery/wait_bailout') === 1 ? 'once' : 'FAIL', "\n";
    for ($i = 0; $i < 100; $i++) {
        $t = $server->getStats()['totals'];
        if ($t['total_requests'] === 2) break;
        delay(10);
    }
    echo 'completed=', $t['total_requests'], ' 2xx=', $t['responses_2xx_total'],
         ' undelivered=', $t['responses_undelivered_total'], "\n";
    $server->stop();
});
$server->start();
$count = 0; $ok = false;
foreach (explode("\n", trim((string) file_get_contents("$tmp/access.log"))) as $line) {
    $a = json_decode($line, true)['Attributes'] ?? [];
    if (($a['url.path'] ?? '') !== '/bailout') continue;
    $count++;
    $ok = !isset($a['http.response.status_code'])
        && ($a['error.type'] ?? '') === 'response_undelivered'
        && ($a['http.response.body.size'] ?? -1) === 0;
}
echo 'bailout log: ', $count === 1 && $ok ? 'once, correct' : 'FAIL', "\n";
?>
--EXPECTF--
%Abailout: terminated
next request: intact
linked wait fault: once
completed=2 2xx=1 undelivered=1
%Abailout log: once, correct
