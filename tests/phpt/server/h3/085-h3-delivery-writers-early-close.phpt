--TEST--
HTTP/3 pool (#351): concurrent writers keep one ordered stream; early close waits for the sender snapshot
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

$tmp = __DIR__ . '/tmp-085-' . getmypid();
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
    if ($req->getPath() === '/early') {
        delay(400);
        $res->setBody('too late');
        return;
    }
    $res->setHeader('content-type', 'text/plain');
    _http_fault_enable('h3/reverse/queue_full', false);
    $writers = [];
    foreach (['A', 'B', 'C'] as $letter) {
        $writers[] = spawn(function () use ($res, $letter) {
            $chunk = str_repeat($letter, 4000);
            if ($letter === 'A') $res->write($chunk);
            else while (!$res->tryWrite($chunk)) {
                if (!$res->awaitWritable(1000)) throw new RuntimeException('writer did not become ready');
            }
        });
        if ($letter === 'A') delay(5);
    }
    spawn(function () { delay(40); _http_fault_disable('h3/reverse/queue_full'); });
    foreach ($writers as $writer) \Async\await($writer);
    $res->end();
});
$client = __DIR__ . '/../../../h3client/h3client';

spawn(function () use ($server, $port, $client, $tmp) {
    delay(600);
    foreach (['plain', 'gzip'] as $path) {
        $env = $path === 'gzip' ? 'H3CLIENT_HEADER=' . escapeshellarg('accept-encoding: gzip') . ' ' : '';
        $cmd = $env . 'H3CLIENT_DEADLINE_MS=3000 ' . escapeshellarg($client)
            . ' 127.0.0.1 ' . $port . ' /' . $path . ' GET 2>' . escapeshellarg("$tmp/stderr");
        $body = (string) shell_exec($cmd);
        if ($path === 'gzip') $body = gzdecode($body);
        $want = str_repeat('A', 4000) . str_repeat('B', 4000) . str_repeat('C', 4000);
        echo "$path writers: ", $body === $want ? 'ordered, intact' : 'FAIL', "\n";
    }
    $out = (string) shell_exec('H3CLIENT_CANCEL_AFTER_MS=40 H3CLIENT_DEADLINE_MS=1500 '
        . escapeshellarg($client) . ' 127.0.0.1 ' . $port . ' ' . escapeshellarg('/early?q=1') . ' GET 2>&1');
    echo 'early close: ', str_contains($out, 'EARLY_CLOSE') ? 'yes' : 'FAIL', "\n";
    for ($i = 0; $i < 100; $i++) {
        $t = $server->getStats()['totals'];
        if ($t['total_requests'] === 3) break;
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
    if (($a['url.path'] ?? '') !== '/early') continue;
    $count++;
    $ok = !isset($a['http.response.status_code'])
        && ($a['error.type'] ?? '') === 'response_undelivered'
        && ($a['http.response.body.size'] ?? -1) === 0
        && ($a['url.query'] ?? '') === 'q=1';
}
echo 'early log: ', $count === 1 && $ok ? 'once, correct identity' : 'FAIL', "\n";
?>
--EXPECTF--
%Aplain writers: ordered, intact
gzip writers: ordered, intact
early close: yes
completed=3 2xx=2 undelivered=1
%Aearly log: once, correct identity
