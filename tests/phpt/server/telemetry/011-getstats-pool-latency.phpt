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
 * than one worker's: offered together they land on different workers, so no
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

    /* Concurrent, not one after another: a worker accepts as fast as it can, so
     * nine requests offered in turn can all be taken by whichever one is free —
     * measured on a macOS runner, where this test then reported a single worker
     * and failed. Nine at once cannot all be accepted by one. */
    $cmds = [];

    for ($i = 0; $i < N; $i++) {
        $path = ($i % 3 === 0) ? '/slow' : '/fast';
        $cmds[] = sprintf('curl -s -o /dev/null --http1.1 --max-time 5 http://127.0.0.1:%d%s &',
                          $port, $path);
    }

    shell_exec(implode(' ', $cmds) . ' wait');

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

    echo 'samples_total=',   (($t['sojourn_samples'] ?? 0) === N ? 1 : 0), "\n";
    echo 'samples_summed=',  (($t['sojourn_samples'] ?? -1) === $samples_sum ? 1 : 0), "\n";
    echo 'service_summed=',  (($t['service_sum_ns'] ?? -1) === $service_sum ? 1 : 0), "\n";
    echo 'max_is_peak=',     (($t['sojourn_max_ns'] ?? -2) === $max_of_max ? 1 : 0), "\n";
    echo 'slow_seen=',       (($t['service_sum_ns'] ?? 0) >= 150 * 1000 * 1000 ? 1 : 0), "\n";
    /* How many workers the nine requests reach is not asserted, because the
     * server does not promise it. Where the kernel has load-balanced
     * SO_REUSEPORT each worker binds its own socket and the connections spread;
     * everywhere else the workers share one descriptor and nothing arbitrates
     * between their reactors, so a busy machine lets one drain the accept queue
     * — measured on Linux under TRUE_ASYNC_SERVER_SHARED_LISTEN_FD=1 pinned to
     * one core: [1,0,8] samples across three workers. The same non-promise is
     * recorded in tests/phpt/websocket/042-topics-interest-filter.phpt. What
     * this test is about holds either way: the totals are the pool's sum rather
     * than one worker's, which the two lines above check. */

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
%ADone
