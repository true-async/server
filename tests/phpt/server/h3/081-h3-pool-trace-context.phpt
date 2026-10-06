--TEST--
HttpServer: pooled HTTP/3 preserves trace context in the handler and access log (#352)
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
use TrueAsync\LogSeverity;
use function Async\spawn;
use function Async\delay;

require __DIR__ . '/_h3_skipif.inc';
require_once __DIR__ . '/../_free_port.inc';

$tmp = __DIR__ . '/tmp-081-' . getmypid();
mkdir($tmp, 0700);
$cert = "$tmp/cert.pem";
$key = "$tmp/key.pem";
register_shutdown_function(function () use ($tmp) {
    foreach (glob("$tmp/*") as $file) { @unlink($file); }
    @rmdir($tmp);
});
if (!h3_gen_cert($key, $cert)) { echo "cert gen failed\n"; exit(1); }

$client = __DIR__ . '/../../../h3client/h3client';
$tid = '0af7651916cd43dd8448eb211c80319c';
$sid = 'b7ad6b7169203331';
$cases = [
    'valid' => "00-$tid-$sid-01",
    'unsampled' => "00-$tid-$sid-00",
    'upper' => strtoupper("00-$tid-$sid-01"),
    'zero' => "00-00000000000000000000000000000000-$sid-01",
    'absent' => null,
];

foreach (['enabled' => true, 'disabled' => false] as $mode => $enabled) {
    $port = tas_free_port_span(2);
    $log = "$tmp/$mode.log";
    $config = (new HttpServerConfig())
        ->addListener('127.0.0.1', $port + 1)
        ->addHttp3Listener('127.0.0.1', $port)
        ->setCertificate($cert)->setPrivateKey($key)
        ->setWorkers(2)
        ->setTelemetryEnabled($enabled)
        ->setLogSinks([
            ['type' => 'file', 'path' => $log, 'format' => 'json',
             'category' => 'access', 'level' => LogSeverity::INFO],
        ]);
    $server = new HttpServer($config);
    $server->addHttpHandler(function ($req, $res) {
        $res->json([
            $req->getTraceParent(), $req->getTraceState(),
            $req->getTraceId(), $req->getSpanId(), $req->getTraceFlags(),
        ]);
    });

    spawn(function () use ($server, $port, $client, $tmp, $mode, $enabled, $cases, $tid, $sid) {
        delay(600);
        foreach ($cases as $name => $tp) {
            $header = $tp === null ? '' : ' H3CLIENT_HEADER=' . escapeshellarg("traceparent: $tp");
            $cmd = 'H3CLIENT_DEADLINE_MS=4000' . $header . ' '
                . escapeshellarg($client) . ' 127.0.0.1 ' . $port . ' /' . $name
                . ' 2>' . escapeshellarg("$tmp/stderr.txt");
            $body = json_decode((string) shell_exec($cmd), true);
            $valid = $enabled && ($name === 'valid' || $name === 'unsampled');
            $want = $valid ? [$tp, null, $tid, $sid, $name === 'valid' ? 1 : 0]
                           : [null, null, null, null, null];
            echo "$mode $name handler: ", $body === $want ? 'ok' : 'FAIL', "\n";
        }
        $server->stop();
    });
    $server->start();

    /* stop() drains the worker access sinks before we read the records. */
    $records = [];
    foreach (explode("\n", trim((string) file_get_contents($log))) as $line) {
        $r = json_decode($line, true);
        $path = $r['Attributes']['url.path'] ?? '';
        if (array_key_exists(ltrim($path, '/'), $cases)) {
            $records[$path][] = $r;
        }
    }
    foreach ($cases as $name => $tp) {
        $rows = $records['/' . $name] ?? [];
        $r = $rows[0] ?? [];
        $valid = $enabled && ($name === 'valid' || $name === 'unsampled');
        $ok = count($rows) === 1 && ($r['Attributes']['network.protocol.version'] ?? '') === '3'
            && ($valid ? (($r['TraceId'] ?? '') === $tid && ($r['SpanId'] ?? '') === $sid)
                       : (!isset($r['TraceId']) && !isset($r['SpanId'])));
        echo "$mode $name log: ", $ok ? 'ok' : 'FAIL', "\n";
    }
}
echo "done\n";
?>
--EXPECTF--
%Aenabled valid handler: ok
enabled unsampled handler: ok
enabled upper handler: ok
enabled zero handler: ok
enabled absent handler: ok
%Aenabled valid log: ok
enabled unsampled log: ok
enabled upper log: ok
enabled zero log: ok
enabled absent log: ok
%Adisabled valid handler: ok
disabled unsampled handler: ok
disabled upper handler: ok
disabled zero handler: ok
disabled absent handler: ok
%Adisabled valid log: ok
disabled unsampled log: ok
disabled upper log: ok
disabled zero log: ok
disabled absent log: ok
done
