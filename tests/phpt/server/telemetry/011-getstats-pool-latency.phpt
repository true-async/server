--TEST--
getStats (#169): request timings are summed across the pool, not kept per worker
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
if (PHP_OS_FAMILY === 'Windows') die('skip libuv on Windows lacks SO_REUSEPORT');
if (!exec('curl --version 2>/dev/null')) die('skip curl CLI not available');
?>
--FILE--
<?php
/* Kept per worker, the four timing fields answered for whichever worker took
 * the scrape: consecutive reads of a PHP metrics endpoint reported 7 ms, then
 * 30 ms, then 7 ms again, and one Prometheus series drawn from four unrelated
 * averages carries no trend. They live in the counter slab now, so the sample
 * count is the pool's and the maximum is the worst any worker saw.
 *
 * The three slow requests are what makes the sum a pool-wide number rather
 * than one worker's: they are spread across workers by the accept hash, so no
 * single slot holds them all. sojourn is the wait before the handler starts,
 * service is the handler itself — the delay shows in the second. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;

require_once __DIR__ . '/../_free_port.inc';

$port = tas_free_port();
const N = 9;

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(5)
    ->setWriteTimeout(5)
    ->setStatsEnabled(true)
    ->setTelemetryEnabled(true)
    ->setWorkers(3);

$server = new HttpServer($config);
$server->addHttpHandler(function ($req, $res) {
    Async\delay($req->getPath() === '/slow' ? 60 : 1);
    $res->setStatusCode(200)->setBody('x')->end();
});

spawn(function () use ($server, $port) {
    usleep(400000);

    for ($i = 0; $i < N; $i++) {
        $path = ($i % 3 === 0) ? '/slow' : '/fast';
        shell_exec(sprintf('curl -s -o /dev/null --http1.1 --max-time 3 http://127.0.0.1:%d%s', $port, $path));
    }

    $stats = [];
    for ($p = 0; $p < 60; $p++) {
        $stats = $server->getStats();
        if (($stats['totals']['total_requests'] ?? 0) >= N) break;
        usleep(20000);
    }

    $t = $stats['totals'];
    $w = array_values($stats['workers']);

    $samples_sum = array_sum(array_column($w, 'sojourn_samples'));
    $service_sum = array_sum(array_column($w, 'service_sum_ns'));
    $max_of_max  = max(array_column($w, 'sojourn_max_ns') ?: [-1]);
    $reporting   = count(array_filter(array_column($w, 'sojourn_samples')));

    echo 'samples_total=',   (($t['sojourn_samples'] ?? 0) === N ? 1 : 0), "\n";
    echo 'samples_summed=',  (($t['sojourn_samples'] ?? -1) === $samples_sum ? 1 : 0), "\n";
    echo 'service_summed=',  (($t['service_sum_ns'] ?? -1) === $service_sum ? 1 : 0), "\n";
    echo 'max_is_peak=',     (($t['sojourn_max_ns'] ?? -2) === $max_of_max ? 1 : 0), "\n";
    echo 'slow_seen=',       (($t['service_sum_ns'] ?? 0) >= 150 * 1000 * 1000 ? 1 : 0), "\n";
    echo 'more_than_one_worker=', ($reporting > 1 ? 1 : 0), "\n";

    $server->stop();
});

$server->start();
echo "Done\n";
?>
--EXPECTF--
samples_total=1
samples_summed=1
service_summed=1
max_is_peak=1
slow_seen=1
more_than_one_worker=1
%ADone
