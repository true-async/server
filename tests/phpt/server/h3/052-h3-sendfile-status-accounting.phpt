--TEST--
sendFile() accounting (#5): h3 ranged send is counted/logged as 206 and returns the range (embedded)
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
require __DIR__ . '/_h3_skipif.inc';
if (PHP_OS_FAMILY === 'Windows') die('skip libuv on Windows lacks SO_REUSEPORT');
h3_skipif(['openssl_cli' => true, 'h3client' => true]);
?>
--ENV--
PHP_HTTP3_DISABLE_RETRY=1
--FILE--
<?php
/* H3 mirror of sendfile/005 on the embedded (non-pool) path: the handler only
 * queues the send, so the status the engine stamps must be what h3_dispose_tail
 * counts and access-logs — 206 for a ranged request, not the handler's 200.
 *
 * The body is checked too: the H3 pump used to ignore the engine's body_offset
 * and answer a ranged request with the head of the file — a 206 carrying the
 * wrong bytes. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\LogSeverity;
use function Async\spawn;

require __DIR__ . '/_h3_skipif.inc';
require_once __DIR__ . '/../_free_port.inc';

$dir = __DIR__ . '/tmp-052';
@mkdir($dir, 0700, true);
$cert = $dir . '/cert.pem';
$key  = $dir . '/key.pem';
$file = $dir . '/payload.bin';
$logPath = $dir . '/access.log';
if (!h3_gen_cert($key, $cert)) { echo "cert gen failed\n"; exit(1); }
/* Position-revealing payload: a wrong offset shows up as wrong bytes. */
file_put_contents($file, str_repeat('ABCDEFGHIJKLMNOPQRSTUVWXYZ', 4));
$fh = fopen($logPath, 'w+b');

register_shutdown_function(function () use ($dir, $cert, $key, $file, $logPath) {
    @unlink($cert); @unlink($key); @unlink($file); @unlink($logPath); @rmdir($dir);
});

$port = tas_free_port_span(2);

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port + 1)
    ->addHttp3Listener('127.0.0.1', $port)
    ->enableTls(true)->setCertificate($cert)->setPrivateKey($key)
    ->setStatsEnabled(true)
    ->setLogSinks([
        ['type' => 'stream', 'stream' => $fh, 'format' => 'json',
         'category' => 'access', 'level' => LogSeverity::INFO],
    ]);

$server = new HttpServer($config);
$server->addHttpHandler(function ($req, $res) use ($file) {
    $res->sendFile($file);
});

$client_bin = __DIR__ . '/../../../h3client/h3client';

spawn(function () use ($server, $port, $client_bin) {
    usleep(400000);

    $run = function (string $path, ?string $range) use ($client_bin, $port) {
        $env = $range !== null ? sprintf('H3CLIENT_HEADER=%s ', escapeshellarg("range: bytes=$range")) : '';
        $body = sys_get_temp_dir() . '/h3-052-body-' . getmypid();
        $cmd = sprintf('%sH3CLIENT_DEADLINE_MS=4000 %s 127.0.0.1 %d %s GET 2>&1 >%s',
            $env, escapeshellarg($client_bin), $port, $path, escapeshellarg($body));
        $err = shell_exec($cmd) ?? '';
        $out = @file_get_contents($body);
        @unlink($body);
        return [preg_match('/STATUS=(\d+)/', $err, $m) ? (int)$m[1] : 0, $out];
    };

    [$st, $body] = $run('/ranged', '32-47');
    echo "wire ranged: $st\n";
    echo "range body:  $body\n";     /* offset 32 of ABC…XYZ×4 */
    [$st] = $run('/full', null);
    echo "wire full:   $st\n";

    $t = [];
    for ($i = 0; $i < 50; $i++) {
        $t = $server->getStats()['totals'];
        if (($t['total_requests'] ?? 0) >= 2) break;
        usleep(20000);
    }
    echo 'total=', ($t['total_requests'] === 2 ? 1 : 0), "\n";
    echo '2xx=',   ($t['responses_2xx_total'] === 2 ? 1 : 0), "\n";

    $server->stop();
});

$server->start();

fclose($fh);
$seen = [];
foreach (explode("\n", trim(file_get_contents($logPath))) as $line) {
    if ($line === '') continue;
    $a = json_decode($line, true)['Attributes'] ?? [];
    $seen[$a['url.path'] ?? '?'] = sprintf('%s/%s', $a['http.response.status_code'] ?? '?',
                                           $a['network.protocol.version'] ?? '?');
}
foreach (['/ranged', '/full'] as $p) {
    echo "log $p: ", $seen[$p] ?? 'missing', "\n";
}
echo "Done\n";
?>
--EXPECT--
wire ranged: 206
range body:  GHIJKLMNOPQRSTUV
wire full:   200
total=1
2xx=1
log /ranged: 206/3
log /full: 200/3
Done
