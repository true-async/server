--TEST--
getStats (#5, A5): status-class counters reconcile with total_requests; H1 conn gauge balances
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
/* Stage A5: every served request classifies into exactly one of
 * responses_{2xx,3xx,4xx,5xx}_total, so the four sum to total_requests.
 * The response status is chosen by path. conns_active_h1 is a gauge that
 * must return to 0 once every (curl, one-shot) connection has closed. */

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
$server->addHttpHandler(function ($req, $res) {
    switch ($req->getPath()) {
        case '/redir':    $res->setStatusCode(301); break;
        case '/notfound': $res->setStatusCode(404); break;
        case '/boom':     $res->setStatusCode(500); break;
        default:          $res->setStatusCode(200); break;
    }
    $res->setBody('x');
});

/* Driven mix. */
$plan = [
    '/'         => 5,
    '/redir'    => 3,
    '/notfound' => 4,
    '/boom'     => 2,
];
$total = array_sum($plan);

spawn(function () use ($port, $server, $plan, $total) {
    usleep(400000);

    foreach ($plan as $path => $n) {
        for ($i = 0; $i < $n; $i++) {
            shell_exec(sprintf('curl -s -o /dev/null --http1.1 --max-time 2 http://127.0.0.1:%d%s', $port, $path));
        }
    }

    $t = [];
    for ($p = 0; $p < 50; $p++) {
        $stats = $server->getStats();
        $t = $stats['totals'];
        if (($t['total_requests'] ?? 0) >= $total && ($t['conns_active_h1'] ?? 1) === 0) {
            break;
        }
        usleep(20000);
    }

    $classes = ($t['responses_2xx_total'] ?? 0)
             + ($t['responses_3xx_total'] ?? 0)
             + ($t['responses_4xx_total'] ?? 0)
             + ($t['responses_5xx_total'] ?? 0);

    echo 'total=',       ($t['total_requests'] === $total ? 1 : 0), "\n";
    echo 'class_sum=',   ($classes === $t['total_requests'] ? 1 : 0), "\n";
    echo '2xx=',         ($t['responses_2xx_total'] === $plan['/'] ? 1 : 0), "\n";
    echo '3xx=',         ($t['responses_3xx_total'] === $plan['/redir'] ? 1 : 0), "\n";
    echo '4xx=',         ($t['responses_4xx_total'] === $plan['/notfound'] ? 1 : 0), "\n";
    echo '5xx=',         ($t['responses_5xx_total'] === $plan['/boom'] ? 1 : 0), "\n";
    echo 'h1_drained=',  ($t['conns_active_h1'] === 0 ? 1 : 0), "\n";
    echo 'no_h2=',       ($t['conns_active_h2'] === 0 ? 1 : 0), "\n";
    echo 'no_h3=',       ($t['conns_active_h3'] === 0 ? 1 : 0), "\n";
    echo "done\n";

    $server->stop();
});

$server->start();
?>
--EXPECTF--
total=1
class_sum=1
2xx=1
3xx=1
4xx=1
5xx=1
h1_drained=1
no_h2=1
no_h3=1
done
%A
