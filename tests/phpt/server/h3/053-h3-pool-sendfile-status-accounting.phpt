--TEST--
sendFile() accounting (#5): reactor-pool ranged send is counted as 206 by the reactor
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
TRUE_ASYNC_SERVER_REACTOR_POOL=1
PHP_HTTP3_DISABLE_RETRY=1
--FILE--
<?php
/* Under the reactor pool the worker only marshals the sendFile; the reactor
 * opens the file and runs the engine, so the final status exists on the
 * transport thread alone. It is counted into the reactor's own counter slice,
 * which getStats() folds into `totals` (and reports under `reactors`) — without
 * that, these requests would be missing from the aggregate entirely.
 *
 * Two deliveries that only work because the H3 body pump is a callback FSM: a
 * file past the 64 KiB inline threshold, and a ranged read (which always takes
 * the pump). Both used to hang here — the pump wanted a PHP coroutine, and a
 * transport reactor has no PHP. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;

require __DIR__ . '/_h3_skipif.inc';
require_once __DIR__ . '/../_free_port.inc';

$dir = __DIR__ . '/tmp-053';
@mkdir($dir, 0700, true);
$cert = $dir . '/cert.pem';
$key  = $dir . '/key.pem';
$file = $dir . '/payload.bin';
$big  = $dir . '/big.bin';
if (!h3_gen_cert($key, $cert)) { echo "cert gen failed\n"; exit(1); }
/* Position-revealing payload: a wrong read offset shows up as wrong bytes. */
file_put_contents($file, str_repeat('ABCDEFGHIJKLMNOPQRSTUVWXYZ', 4));
/* Past SEND_FILE_SLURP_THRESHOLD (64 KiB), so delivery goes through the pump. */
$bigBody = str_repeat('z', 200000);
file_put_contents($big, $bigBody);

register_shutdown_function(function () use ($dir, $cert, $key, $file, $big) {
    @unlink($cert); @unlink($key); @unlink($file); @unlink($big); @rmdir($dir);
});

$port = tas_free_port_span(2);

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port + 1)
    ->addHttp3Listener('127.0.0.1', $port)
    ->enableTls(true)->setCertificate($cert)->setPrivateKey($key)
    ->setStatsEnabled(true)
    ->setWorkers(2);

$server = new HttpServer($config);
$server->addHttpHandler(function ($req, $res) use ($file, $big) {
    $res->sendFile($req->getPath() === '/big' ? $big : $file);
});

$client_bin = __DIR__ . '/../../../h3client/h3client';

spawn(function () use ($server, $port, $client_bin, $bigBody) {
    usleep(600000);

    $run = function (string $path, ?string $range) use ($client_bin, $port) {
        $env = $range !== null ? sprintf('H3CLIENT_HEADER=%s ', escapeshellarg("range: bytes=$range")) : '';
        $out = sys_get_temp_dir() . '/h3-053-body-' . getmypid();
        $cmd = sprintf('%sH3CLIENT_DEADLINE_MS=8000 %s 127.0.0.1 %d %s GET 2>&1 >%s',
            $env, escapeshellarg($client_bin), $port, $path, escapeshellarg($out));
        $err = shell_exec($cmd) ?? '';
        $body = @file_get_contents($out);
        @unlink($out);
        return [preg_match('/STATUS=(\d+)/', $err, $m) ? (int)$m[1] : 0, (string)$body];
    };

    [$st, $body] = $run('/ranged', '32-47');
    echo "wire ranged: $st\n";
    echo "range body:  $body\n";
    [$st] = $run('/full', null);
    echo "wire full:   $st\n";
    [$st, $body] = $run('/big', null);
    echo "wire big:    $st\n";
    echo "big intact:  ", ($body === $bigBody ? 1 : 0), " (", strlen($body), " bytes)\n";

    $t = [];
    $stats = [];
    for ($i = 0; $i < 60; $i++) {
        $stats = $server->getStats();
        $t = $stats['totals'];
        if (($t['total_requests'] ?? 0) >= 3) break;
        usleep(50000);
    }

    $classes = $t['responses_2xx_total'] + $t['responses_3xx_total']
             + $t['responses_4xx_total'] + $t['responses_5xx_total'];

    echo 'total=',      ($t['total_requests'] === 3 ? 1 : 0), "\n";
    echo 'class_sum=',  ($classes === $t['total_requests'] ? 1 : 0), "\n";
    echo '2xx=',        ($t['responses_2xx_total'] === 3 ? 1 : 0), "\n";
    /* The reactor is where they were counted — it must be the one reporting them. */
    $reactor_total = 0;
    foreach ($stats['reactors'] ?? [] as $r) { $reactor_total += $r['total_requests']; }
    echo 'from_reactors=', ($reactor_total === 3 ? 1 : 0), "\n";
    echo "done\n";

    /* Pool-parent stop() is unsupported (#11); %A absorbs its exception. */
    $server->stop();
});

$server->start();
?>
--EXPECTF--
%Awire ranged: 206
range body:  GHIJKLMNOPQRSTUV
wire full:   200
wire big:    200
big intact:  1 (200000 bytes)
total=1
class_sum=1
2xx=1
from_reactors=1
done
%A
