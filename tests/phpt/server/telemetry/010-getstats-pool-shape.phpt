--TEST--
getStats (#5): under a worker pool, totals are exactly the sum of the reported workers
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
/* `totals` is what a scraper exports, `workers` is what it drills into — they
 * must agree, or the two views of the same server disagree. Requests land on
 * whichever worker the kernel picked, so this asserts the identity
 * (sum over workers + reactors == totals) rather than any single worker's share.
 * Guards the aggregate against a counter that is summed in one place and
 * forgotten in the other. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;

require_once __DIR__ . '/../_free_port.inc';

$port = tas_free_port();
const N = 8;

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(5)
    ->setWriteTimeout(5)
    ->setStatsEnabled(true)
    ->setWorkers(3);

$server = new HttpServer($config);
$server->addHttpHandler(function ($req, $res) {
    $res->setStatusCode($req->getPath() === '/bad' ? 500 : 200)->setBody('x')->end();
});

spawn(function () use ($server, $port) {
    usleep(400000);

    for ($i = 0; $i < N; $i++) {
        $path = ($i % 4 === 0) ? '/bad' : '/ok';
        shell_exec(sprintf('curl -s -o /dev/null --http1.1 --max-time 2 http://127.0.0.1:%d%s', $port, $path));
    }

    $stats = [];
    for ($p = 0; $p < 60; $p++) {
        $stats = $server->getStats();
        if (($stats['totals']['total_requests'] ?? 0) >= N) break;
        usleep(20000);
    }

    $t = $stats['totals'];

    /* The identity, key by key: totals == sum(workers) + sum(reactors). Gauges
     * are summed the same way, so a live gauge must not break it either. */
    $sum = [];
    foreach (array_merge(array_values($stats['workers']), array_values($stats['reactors'])) as $slot) {
        foreach ($slot as $k => $v) {
            $sum[$k] = ($sum[$k] ?? 0) + $v;
        }
    }

    $mismatch = [];
    foreach ($t as $k => $v) {
        /* h2_ping_rtt_ns is a latest-sample gauge: it reports the max across
         * workers, not their sum — summing round-trip times means nothing. */
        if ($k === 'h2_ping_rtt_ns') continue;
        if (($sum[$k] ?? 0) !== $v) $mismatch[] = $k;
    }

    echo 'workers=',   (count($stats['workers']) === 3 ? 1 : 0), "\n";
    echo 'total=',     ($t['total_requests'] === N ? 1 : 0), "\n";
    echo '5xx=',       ($t['responses_5xx_total'] === 2 ? 1 : 0), "\n";
    echo '2xx=',       ($t['responses_2xx_total'] === N - 2 ? 1 : 0), "\n";
    echo 'aggregate_mismatch=', $mismatch ? implode(',', $mismatch) : 'none', "\n";
    echo "done\n";

    $server->stop();
});

$server->start();
?>
--EXPECTF--
workers=1
total=1
5xx=1
2xx=1
aggregate_mismatch=none
done
%A
