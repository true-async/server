--TEST--
HTTP/3 pool (#351): a refused nonblocking first write is retryable; a queue deadline resets through control
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
$tmp = __DIR__ . '/tmp-084-' . getmypid();
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
    ->setWorkers(2)->setStatsEnabled(true)->setWriteTimeout(1)
    ->setLogSinks([['type' => 'file', 'path' => "$tmp/access.log", 'format' => 'json',
                   'category' => 'access', 'level' => LogSeverity::INFO]]);
$server = new HttpServer($config);
$server->addHttpHandler(function ($req, $res) {
    _http_fault_enable('h3/reverse/queue_full', false);
    if ($req->getPath() === '/try') {
        $res->setHeader('content-type', 'text/plain');
        $res->setHeader('vary', ['Origin', 'X-Probe']);
        if ($req->getHeader('accept-encoding') === null) $res->setHeader('content-length', '8');
        $yielded = false;
        spawn(function () use (&$yielded) { $yielded = true; });
        $start = hrtime(true);
        try { $offered = $res->tryWrite('refused!'); }
        finally { _http_fault_disable('h3/reverse/queue_full'); }
        /* A refused first offer must not commit the headers or the codec. */
        $res->setHeader('x-rollback', $res->getHeaders()['vary'] === ['Origin', 'X-Probe']
            && $res->getHeader('content-length') === ($req->getHeader('accept-encoding') === null ? '8' : null) ? 'yes' : 'FAIL');
        $res->setHeader('x-first-refused', $offered ? 'FAIL' : 'yes');
        $res->setHeader('x-nonblocking', !$yielded && (hrtime(true) - $start) < 250000000 ? 'yes' : 'FAIL');
        $res->write('accepted')->end();
    } else {
        $res->setBody('must-not-leave');
        /* Leave FULL enabled: the sender's deadline must terminate the client
         * via the separate control route, not another data-queue message. */
    }
});
$client = __DIR__ . '/../../../h3client/h3client';
spawn(function () use ($server, $port, $client, $tmp) {
    delay(600);
    foreach (['plain', 'gzip'] as $codec) {
        $env = $codec === 'gzip' ? 'H3CLIENT_HEADER=' . escapeshellarg('accept-encoding: gzip') . ' ' : '';
        $body = (string) shell_exec($env . 'H3CLIENT_PRINT_HEADERS=1 H3CLIENT_DEADLINE_MS=2500 '
            . escapeshellarg($client) . ' 127.0.0.1 ' . $port . ' /try GET 2>' . escapeshellarg("$tmp/try.stderr"));
        $err = (string) file_get_contents("$tmp/try.stderr");
        if ($codec === 'gzip') $body = gzdecode($body);
        echo "nonblocking $codec: ", str_contains($err, 'STATUS=200')
            && str_contains($err, 'HDR x-first-refused: yes')
            && str_contains($err, 'HDR x-nonblocking: yes')
            && str_contains($err, 'HDR x-rollback: yes')
            && ($codec !== 'gzip' || str_contains($err, 'HDR content-encoding: gzip'))
            && $body === 'accepted' ? 'retryable, no yield, intact' : 'FAIL', "\n";
    }
    $deadlineStart = hrtime(true);
    $out = (string) shell_exec('H3CLIENT_DEADLINE_MS=2500 ' . escapeshellarg($client)
        . ' 127.0.0.1 ' . $port . ' /timeout GET 2>&1');
    $deadlineMs = (hrtime(true) - $deadlineStart) / 1000000;
    _http_fault_disable('h3/reverse/queue_full');
    echo 'deadline: ', !str_contains($out, 'STATUS=200')
        && !str_contains($out, 'timeout (completed') && $deadlineMs >= 800 && $deadlineMs < 2300 ? 'terminated at deadline' : 'FAIL', "\n";
    for ($i = 0; $i < 100; $i++) {
        $t = $server->getStats()['totals'];
        if ($t['total_requests'] === 3) break;
        delay(10);
    }
    echo 'completed=', $t['total_requests'], ' 2xx=', $t['responses_2xx_total'],
         ' undelivered=', $t['responses_undelivered_total'],
         ' dropped=', $t['worker_wire_dropped_total'], "\n";
    $server->stop();
});
$server->start();
$count = 0; $ok = false;
foreach (explode("\n", trim((string) file_get_contents("$tmp/access.log"))) as $line) {
    $a = json_decode($line, true)['Attributes'] ?? [];
    if (($a['url.path'] ?? '') !== '/timeout') continue;
    $count++;
    $ok = !isset($a['http.response.status_code'])
        && ($a['error.type'] ?? '') === 'response_undelivered'
        && ($a['http.response.body.size'] ?? -1) === 0;
}
echo 'deadline log: ', $count === 1 && $ok ? 'once, no status, zero bytes' : 'FAIL', "\n";
?>
--EXPECTF--
%Anonblocking plain: retryable, no yield, intact
nonblocking gzip: retryable, no yield, intact
deadline: terminated at deadline
completed=3 2xx=2 undelivered=1 dropped=1
%Adeadline log: once, no status, zero bytes
