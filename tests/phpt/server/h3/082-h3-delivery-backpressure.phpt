--TEST--
HTTP/3 pool (#351): FULL backpressure yields the coroutine; render/submit failures are not delivered responses
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

$tmp = __DIR__ . '/tmp-082-' . getmypid();
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
$server->addHttpHandler(function ($req, $res) use ($tmp) {
    if ($req->getPath() === '/pressure') {
        _http_fault_enable('h3/reverse/queue_full', false);
        /* This task must run on the SAME worker while the FULL sender waits.
         * Ending the sender early cancels its request scope and this task. */
        spawn(function () use ($tmp) {
            delay(40);
            file_put_contents("$tmp/progress", 'yes');
            _http_fault_disable('h3/reverse/queue_full');
        });
    }
    if ($req->getPath() === '/held') {
        static $held = [];
        $held[] = $req; /* lastref must not be the terminal-outcome signal */
    }
    $res->setBody('body-' . $req->getPath());
});
$client = __DIR__ . '/../../../h3client/h3client';

spawn(function () use ($server, $port, $client, $tmp) {
    delay(600);
    foreach (['pressure', 'render', 'submit', 'held'] as $path) {
        if ($path === 'render') _http_fault_enable('h3/delivery/render_failed', true);
        if ($path === 'submit') _http_fault_enable('h3/delivery/submit_failed', true);
        $out = (string) shell_exec('H3CLIENT_DEADLINE_MS=3000 ' . escapeshellarg($client)
            . ' 127.0.0.1 ' . $port . ' /' . $path . ' GET 2>&1');
        if ($path === 'pressure' || $path === 'held') {
            echo "$path: ", str_contains($out, 'STATUS=200')
                && str_contains($out, 'body-/' . $path) ? 'intact' : 'FAIL', "\n";
        } else {
            echo "$path: ", !str_contains($out, 'STATUS=200')
                && !str_contains($out, 'timeout') ? 'terminated' : 'FAIL', "\n";
        }
    }
    echo 'same-worker progress: ', is_file("$tmp/progress") ? 'yes' : 'FAIL', "\n";
    echo 'FULL exercised: ', _http_fault_hits('h3/reverse/queue_full') > 1 ? 'yes' : 'FAIL', "\n";
    for ($i = 0; $i < 100; $i++) {
        $t = $server->getStats()['totals'];
        if ($t['total_requests'] === 4) break;
        delay(10);
    }
    echo 'completed=', $t['total_requests'], ' 2xx=', $t['responses_2xx_total'],
         ' undelivered=', $t['responses_undelivered_total'],
         ' dropped=', $t['worker_wire_dropped_total'], "\n";
    $server->stop();
});
$server->start();

$rows = [];
foreach (explode("\n", trim((string) file_get_contents("$tmp/access.log"))) as $line) {
    $r = json_decode($line, true);
    $a = $r['Attributes'] ?? [];
    $rows[$a['url.path'] ?? ''][] = $a;
}
foreach (['pressure', 'render', 'submit', 'held'] as $path) {
    $records = $rows['/' . $path] ?? [];
    $a = $records[0] ?? [];
    $ok = count($records) === 1;
    if ($path === 'render' || $path === 'submit') {
        $ok = $ok && !isset($a['http.response.status_code'])
            && ($a['error.type'] ?? '') === 'response_undelivered'
            && ($a['http.response.body.size'] ?? -1) === 0;
    } else {
        $ok = $ok && ($a['http.response.status_code'] ?? 0) === 200
            && !isset($a['error.type'])
            && ($a['http.response.body.size'] ?? -1) === strlen('body-/' . $path);
    }
    echo "log $path: ", $ok ? 'once, correct' : 'FAIL', "\n";
    if (!$ok) echo json_encode($records), "\n";
}
?>
--EXPECTF--
%Apressure: intact
render: terminated
submit: terminated
held: intact
same-worker progress: yes
FULL exercised: yes
completed=4 2xx=2 undelivered=2 dropped=1
%Alog pressure: once, correct
log render: once, correct
log submit: once, correct
log held: once, correct
