--TEST--
HTTP/3 pool (#351): sendFile pump failure after its first chunk resets and reports one failure
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

$tmp = __DIR__ . '/tmp-091-' . getmypid();
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
$server->addHttpHandler(function ($req, $res) use ($tmp) { $res->sendFile("$tmp/body.bin"); });
$client = __DIR__ . '/../../../h3client/h3client';
spawn(function () use ($server, $port, $client, $tmp, $fileBody) {
    delay(600);
    _http_fault_enable('h3/sendfile/read_failed_after_chunk', true);
    $out = (string) shell_exec('H3CLIENT_DEADLINE_MS=3000 ' . escapeshellarg($client)
        . ' 127.0.0.1 ' . $port . ' /file GET 2>' . escapeshellarg("$tmp/stderr"));
    $err = (string) file_get_contents("$tmp/stderr");
    echo 'pump fault=', _http_fault_hits('h3/sendfile/read_failed_after_chunk'), "\n";
    echo 'peer: ', str_contains($err, 'RESET=') && !str_contains($err, 'timeout')
        && strlen($out) < strlen($fileBody) ? 'reset, incomplete' : 'FAIL', "\n";
    if (!str_contains($err, 'RESET=')) echo 'client diagnostic: ', $err, "\n";
    for ($i = 0; $i < 200; $i++) {
        $t = $server->getStats()['totals'];
        if ($t['total_requests'] === 1) break;
        delay(5);
    }
    echo 'outcome: ', $t['total_requests'] === 1
        && $t['responses_aborted_total'] + $t['responses_undelivered_total'] === 1 ? 'one failure' : 'FAIL', "\n";
    $server->stop();
});
$server->start();
$rows = [];
foreach (explode("\n", trim((string) file_get_contents("$tmp/access.log"))) as $line) {
    $a = json_decode($line, true)['Attributes'] ?? [];
    if (($a['url.path'] ?? '') === '/file') $rows[] = $a;
}
$a = $rows[0] ?? [];
$type = isset($a['http.response.status_code']) ? 'response_aborted' : 'response_undelivered';
echo 'access: ', count($rows) === 1 && ($a['error.type'] ?? '') === $type
    && ($a['http.response.body.size'] ?? -1) < strlen($fileBody) ? 'once, failed' : 'FAIL', "\n";
?>
--EXPECTF--
%Apump fault=1
peer: reset, incomplete
outcome: one failure
%Aaccess: once, failed
