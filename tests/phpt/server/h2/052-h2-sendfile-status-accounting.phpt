--TEST--
sendFile() accounting (#5): h2 ranged send is counted/logged as 206, a failed send as 500
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
require __DIR__ . '/_h2_skipif.inc';
h2_skipif(['curl_h2' => true]);
?>
--FILE--
<?php
/* H2 mirror of sendfile/005: the status the engine stamps must be the one that
 * is counted and access-logged, not the handler's default 200. H2 accounts a
 * sendFile at h2_stream_dispose_tail, which the engine's on_done drives. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\LogSeverity;
use function Async\spawn;
use function Async\await;

require_once __DIR__ . '/../_free_port.inc';

$tmp = tempnam(sys_get_temp_dir(), 'sf-h2-');
file_put_contents($tmp, "0123456789");
$logPath = sys_get_temp_dir() . '/php-http-h2-052-' . getmypid() . '.log';
@unlink($logPath);
$fh = fopen($logPath, 'w+b');
register_shutdown_function(function () use ($tmp, $logPath) {
    @unlink($tmp);
    @unlink($logPath);
});

$port = tas_free_port();

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(5)
    ->setWriteTimeout(5)
    ->setStatsEnabled(true)
    ->setLogSinks([
        ['type' => 'stream', 'stream' => $fh, 'format' => 'json',
         'category' => 'access', 'level' => LogSeverity::INFO],
    ]);

$server = new HttpServer($config);
$server->addHttpHandler(function ($req, $res) use ($tmp) {
    $res->sendFile($req->getPath() === '/missing' ? $tmp . '.nope' : $tmp);
});

$client = spawn(function () use ($port, $server) {
    usleep(30000);

    foreach ([['/ranged', '-H "Range: bytes=0-3"'], ['/full', ''], ['/missing', '']] as [$p, $extra]) {
        exec(sprintf(
            'curl --http2-prior-knowledge -sS -o /dev/null -w "%%{http_code}" --max-time 3 %s http://127.0.0.1:%d%s 2>&1',
            $extra, $port, $p), $out, $rc);
        echo "wire $p: ", end($out), "\n";
    }

    $t = [];
    for ($i = 0; $i < 50; $i++) {
        $t = $server->getStats()['totals'];
        if (($t['total_requests'] ?? 0) >= 3) break;
        usleep(20000);
    }
    $classes = $t['responses_2xx_total'] + $t['responses_3xx_total']
             + $t['responses_4xx_total'] + $t['responses_5xx_total'];

    echo 'total=',     ($t['total_requests'] === 3 ? 1 : 0), "\n";
    echo 'class_sum=', ($classes === $t['total_requests'] ? 1 : 0), "\n";

    $server->stop();
});

$server->start();
await($client);

fclose($fh);
$seen = [];
foreach (explode("\n", trim(file_get_contents($logPath))) as $line) {
    if ($line === '') continue;
    $a = json_decode($line, true)['Attributes'] ?? [];
    $seen[$a['url.path'] ?? '?'] = sprintf('%s/%s', $a['http.response.status_code'] ?? '?',
                                           $a['network.protocol.version'] ?? '?');
}
echo 'log lines=', count($seen), "\n";
foreach (['/ranged', '/full', '/missing'] as $p) {
    echo "log $p: ", $seen[$p] ?? 'missing', "\n";
}
echo "Done\n";
--EXPECT--
wire /ranged: 206
wire /full: 200
wire /missing: 500
total=1
class_sum=1
log lines=3
log /ranged: 206/2
log /full: 200/2
log /missing: 500/2
Done
