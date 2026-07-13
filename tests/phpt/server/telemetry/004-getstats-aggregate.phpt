--TEST--
getStats (#5, A4): throws when disabled; aggregates per-worker slabs when enabled
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
/* Stage A4: HttpServer::getStats() is the cross-worker aggregate. Disabled -> it
 * throws. Enabled -> it walks the slab and returns {enabled, workers, totals};
 * the per-worker totals must sum to the number of requests served. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;

require_once __DIR__ . '/../_free_port.inc';

/* Disabled -> throws. */
$off = new HttpServer((new HttpServerConfig())->addListener('127.0.0.1', tas_free_port()));
try {
    $off->getStats();
    echo "throws_when_disabled=0\n";
} catch (\Throwable $e) {
    echo 'throws_when_disabled=', (str_contains($e->getMessage(), 'not enabled') ? 1 : 0), "\n";
}

/* Enabled but no pool (single server): getStats reports the one server as
 * worker 0 with zeroed counters (nothing served yet). Covers the non-slab path. */
$one = new HttpServer((new HttpServerConfig())
    ->addListener('127.0.0.1', tas_free_port())
    ->setStatsEnabled(true));
$s1 = $one->getStats();
echo 'single_enabled=', ($s1['enabled'] === true ? 1 : 0), "\n";
echo 'single_worker_count=', count($s1['workers']), "\n";
echo 'single_zero=', (($s1['totals']['total_requests'] ?? -1) === 0 ? 1 : 0), "\n";

/* Enabled pool -> aggregate. */
$port    = tas_free_port();
$workers = 2;
$reqs    = 16;

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(5)
    ->setWriteTimeout(5)
    ->setStatsEnabled(true)
    ->setWorkers($workers);

$server = new HttpServer($config);
$server->addHttpHandler(fn ($req, $res) => $res->setStatusCode(200)->setBody('OK'));

spawn(function () use ($port, $server, $workers, $reqs) {
    usleep(400000);

    $hits = 0;
    for ($i = 0; $i < $reqs; $i++) {
        if ((string) shell_exec(sprintf('curl -s --max-time 2 http://127.0.0.1:%d/', $port)) === 'OK') {
            $hits++;
        }
    }

    $stats = [];
    for ($p = 0; $p < 50; $p++) {
        $stats = $server->getStats();
        if (($stats['totals']['total_requests'] ?? 0) >= $hits) {
            break;
        }
        usleep(20000);
    }

    echo 'enabled=', ($stats['enabled'] === true ? 1 : 0), "\n";
    echo 'served=', ($hits === $reqs ? 1 : 0), "\n";
    echo 'worker_count=', count($stats['workers'] ?? []), "\n";
    echo 'totals_match=', (($stats['totals']['total_requests'] ?? -1) === $hits ? 1 : 0), "\n";
    echo "done\n";

    $server->stop();
});

$server->start();
?>
--EXPECTF--
throws_when_disabled=1
single_enabled=1
single_worker_count=1
single_zero=1
enabled=1
served=1
worker_count=2
totals_match=1
done
%A
