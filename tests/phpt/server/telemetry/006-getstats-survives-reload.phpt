--TEST--
getStats (#5): monotonic totals survive a pool reload; gauges do not linger
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
/* A worker's slab slot is wiped when the next worker claims it, so a reload()
 * used to reset every total to zero — getStats() would report total_requests
 * running BACKWARDS, which any metrics scraper reads as a counter reset.
 *
 * Retiring a worker now folds its monotonic totals into the registry, so the
 * aggregate only ever grows. Gauges (active_requests, conns_active_*) must NOT
 * be inherited: a dead worker holds no open connections, and carrying its last
 * value forward would strand a phantom that never drains. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;

require_once __DIR__ . '/../_free_port.inc';

$port = tas_free_port();

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(5)
    ->setWriteTimeout(5)
    ->setStatsEnabled(true)
    ->setWorkers(2);

$server = new HttpServer($config);
$server->addHttpHandler(fn ($req, $res) => $res->setStatusCode(200)->setBody('OK'));

$hit = function (int $n) use ($port): int {
    $ok = 0;
    for ($i = 0; $i < $n; $i++) {
        if ((string) shell_exec(sprintf('curl -s --max-time 2 http://127.0.0.1:%d/', $port)) === 'OK') {
            $ok++;
        }
    }
    return $ok;
};

$settle = function (HttpServer $s, int $atLeast): array {
    for ($p = 0; $p < 50; $p++) {
        $st = $s->getStats();
        if (($st['totals']['total_requests'] ?? 0) >= $atLeast) {
            return $st;
        }
        usleep(20000);
    }
    return $s->getStats();
};

spawn(function () use ($server, $hit, $settle) {
    usleep(400000);

    $before = $hit(8);
    $s1     = $settle($server, $before);
    $t1     = $s1['totals']['total_requests'] ?? -1;
    echo 'before_reload_ok=', ($t1 === $before ? 1 : 0), "\n";

    $server->reload();
    usleep(600000);   /* old cohort exits, new one claims the slots */

    /* The aggregate must not have dropped just because the workers rotated. */
    $mid = $server->getStats();
    echo 'survives_reload=', (($mid['totals']['total_requests'] ?? -1) >= $t1 ? 1 : 0), "\n";

    /* A retired worker's connection gauge must not be inherited. */
    echo 'no_phantom_conns=', (($mid['totals']['conns_active_h1'] ?? -1) === 0 ? 1 : 0), "\n";
    echo 'no_phantom_reqs=', (($mid['totals']['active_requests'] ?? -1) === 0 ? 1 : 0), "\n";
    

    $after = $hit(8);
    $s2    = $settle($server, $t1 + $after);
    $t2    = $s2['totals']['total_requests'] ?? -1;

    /* Counts from both cohorts add up — nothing was lost with the dead workers. */
    echo 'accumulates=', ($t2 === $t1 + $after ? 1 : 0), "\n";
    echo 'monotonic=', ($t2 >= $t1 ? 1 : 0), "\n";
    
    echo "done\n";

    $server->stop();
});

$server->start();
?>
--EXPECTF--
before_reload_ok=1
%A
survives_reload=1
no_phantom_conns=1
no_phantom_reqs=1
accumulates=1
monotonic=1
done
%A
