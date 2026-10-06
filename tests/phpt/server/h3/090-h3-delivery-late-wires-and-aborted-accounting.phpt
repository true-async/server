--TEST--
HTTP/3 pool (#351): late CHUNK, END, ABORT and SEND_FILE survive FULL; acknowledged abort is counted once
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
require __DIR__ . '/_h3_skipif.inc';
h3_skipif(['openssl_cli' => true, 'h3client' => true]);
if (!function_exists('_http_fault_enable') || !function_exists('_http_server_response_acked_body')) die('skip fault injection and test hooks required');
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

$tmp = __DIR__ . '/tmp-090-' . getmypid();
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
$fileBody = str_repeat('sendfile-0123456789', 8192);
file_put_contents("$tmp/body.bin", $fileBody);
$server = new HttpServer($config);
$server->addHttpHandler(function ($req, $res) use ($tmp) {
    $path = substr($req->getPath(), 1);
    if ($path !== 'headers' && $path !== 'file') {
        $res->write('prefix-');
        if ($path === 'abort') {
            for ($i = 0; $i < 200 && _http_server_response_acked_body($res) < 7; $i++) delay(5);
            if (_http_server_response_acked_body($res) !== 7) throw new RuntimeException('ACK barrier failed');
        }
    }
    _http_fault_enable('h3/reverse/queue_full', false);
    spawn(function () use ($tmp, $path) {
        delay(60);
        file_put_contents("$tmp/released-$path", 'yes');
        _http_fault_disable('h3/reverse/queue_full');
    });
    if ($path === 'file') $res->sendFile("$tmp/body.bin", new TrueAsync\SendFileOptions(
        contentType: 'application/x-delivery-test', downloadName: 'result.bin',
        cacheControl: 'private, max-age=60'));

    elseif ($path === 'abort') $res->abort(258);
    else {
        if ($path === 'headers') $res->write('prefix-');
        if ($path === 'chunk') $res->write('chunk');
        $res->setTrailer('x-terminal', $path);
        $res->end();
    }
});
$client = __DIR__ . '/../../../h3client/h3client';
spawn(function () use ($server, $port, $client, $tmp, $fileBody) {
    delay(600);
    foreach (['headers', 'chunk', 'end', 'abort', 'file'] as $path) {
        $out = (string) shell_exec('H3CLIENT_PRINT_HEADERS=1 H3CLIENT_DEADLINE_MS=3000 '
            . escapeshellarg($client) . ' 127.0.0.1 ' . $port . ' /' . $path
            . ' GET 2>' . escapeshellarg("$tmp/stderr"));
        $err = (string) file_get_contents("$tmp/stderr");
        $full = _http_fault_hits('h3/reverse/queue_full') > 0
            && file_exists("$tmp/released-$path");
        if ($path === 'abort') $ok = str_contains($err, 'RESET=258') && $out === 'prefix-';
        elseif ($path === 'file') $ok = $out === $fileBody
            && str_contains($err, 'HDR content-type: application/x-delivery-test')
            && str_contains($err, 'filename="result.bin"')
            && str_contains($err, 'HDR cache-control: private, max-age=60');
        else $ok = $out === 'prefix-' . ($path === 'chunk' ? 'chunk' : '')
            && str_contains($err, 'HDR x-terminal: ' . $path);
        echo "$path: ", $full && $ok ? 'FULL exercised, correct' : 'FAIL', "\n";
    }
    for ($i = 0; $i < 200; $i++) {
        $t = $server->getStats()['totals'];
        if ($t['total_requests'] === 5) break;
        delay(5);
    }
    echo 'completed=', $t['total_requests'], ' 2xx=', $t['responses_2xx_total'],
        ' aborted=', $t['responses_aborted_total'], ' undelivered=', $t['responses_undelivered_total'], "\n";
    $server->stop();
});
$server->start();
$rows = [];
foreach (explode("\n", trim((string) file_get_contents("$tmp/access.log"))) as $line) {
    $a = json_decode($line, true)['Attributes'] ?? [];
    $rows[$a['url.path'] ?? ''][] = $a;
}
foreach (['headers', 'chunk', 'end', 'abort', 'file'] as $path) {
    $a = $rows['/' . $path][0] ?? [];
    $want = $path === 'file' ? strlen($fileBody) : ($path === 'chunk' ? 12 : 7);
    $ok = count($rows['/' . $path] ?? []) === 1 && ($a['http.response.status_code'] ?? 0) === 200
        && ($a['http.response.body.size'] ?? -1) === $want
        && ($path === 'abort' ? ($a['error.type'] ?? '') === 'response_aborted' : !isset($a['error.type']));
    echo "log $path: ", $ok ? 'once, correct' : 'FAIL', "\n";
}
?>
--EXPECTF--
%Aheaders: FULL exercised, correct
chunk: FULL exercised, correct
end: FULL exercised, correct
abort: FULL exercised, correct
file: FULL exercised, correct
completed=5 2xx=5 aborted=1 undelivered=0
%Alog headers: once, correct
log chunk: once, correct
log end: once, correct
log abort: once, correct
log file: once, correct
