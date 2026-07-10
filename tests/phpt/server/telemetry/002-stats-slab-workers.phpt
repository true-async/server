--TEST--
Stats slab (#5, A2): pool workers bump their own slab slot, not an embedded counter
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
if (PHP_OS_FAMILY === 'Windows') die('skip libuv on Windows lacks SO_REUSEPORT');
if (!function_exists('_http_server_stats_slab_snapshot')) {
    die('skip built without --enable-http-server-test-hooks');
}
if (!exec('curl --version 2>/dev/null')) die('skip curl CLI not available');
?>
--FILE--
<?php
/* Stage A2: each pool worker claims a stats-slab slot at start() and points its
 * live counters at it. Drive real requests through a 2-worker pool, then read
 * the slab from the parent coroutine (same process — the slab is process-wide).
 * Each worker must have an active slot, and the per-slot total_requests must sum
 * to the number served — proving bumps land in the slab, not an embedded copy. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;

require_once __DIR__ . '/../_free_port.inc';

$port    = tas_free_port();
$workers = 2;
$reqs    = 16;

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(5)
    ->setWriteTimeout(5)
    ->setWorkers($workers);

$server = new HttpServer($config);
$server->addHttpHandler(function ($req, $res) {
    $res->setStatusCode(200)->setBody('OK');
});

spawn(function () use ($port, $server, $workers, $reqs) {
    usleep(400000); /* workers thread up + bind */

    $hits = 0;
    for ($i = 0; $i < $reqs; $i++) {
        $out = (string) shell_exec(sprintf('curl -s --max-time 2 http://127.0.0.1:%d/', $port));
        if ($out === 'OK') {
            $hits++;
        }
    }

    /* Poll the slab until the workers' bumps settle (bounded). */
    $slots = [];
    for ($p = 0; $p < 50; $p++) {
        $slots = _http_server_stats_slab_snapshot();
        if (array_sum($slots) >= $hits) {
            break;
        }
        usleep(20000);
    }

    echo 'served=', ($hits === $reqs ? 1 : 0), "\n";
    echo 'active_slots=', count($slots), "\n";
    echo 'sum_matches=', (array_sum($slots) === $hits ? 1 : 0), "\n";
    echo "done\n";

    $server->stop();
});

$server->start();
?>
--EXPECTF--
served=1
active_slots=2
sum_matches=1
done
%A
