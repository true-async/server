--TEST--
getStats: the connection gauge and the parse-error counters are the pool's, not one worker's
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
if (PHP_OS_FAMILY === 'Windows') die('skip libuv on Windows lacks SO_REUSEPORT');
?>
--FILE--
<?php
/* Twenty-two counters lived on the server object rather than in the counter
 * slab, so getStats() never reported them and a pool had no way to add them
 * up: whichever object answered the scrape held its own count, and the other
 * workers' connections and parse errors were invisible. They are slab fields
 * now, which is what makes both halves below a pool-wide number.
 *
 * The two halves cover the two kinds the table distinguishes: active_connections
 * is a GAUGE, summed across live workers only, and parse_errors_400_total is a
 * SUM that survives a worker retiring. Six of each, offered together so the
 * kernel spreads them over the three workers — no single slot holds them all. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;

require_once __DIR__ . '/../_free_port.inc';

const CONNS = 6;

$port = tas_free_port();

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(15)
    ->setWriteTimeout(15)
    ->setStatsEnabled(true)
    ->setWorkers(3);

$server = new HttpServer($config);
$server->addHttpHandler(function ($req, $res) {
    $res->setStatusCode(200)->setBody('x')->end();
});

spawn(function () use ($server, $port) {
    usleep(400000);

    /* Held open with nothing written: the gauge counts an accepted connection,
     * and no request is what keeps the handler out of the measurement. */
    $held = [];
    for ($i = 0; $i < CONNS; $i++) {
        $held[] = stream_socket_client("tcp://127.0.0.1:$port", $e, $m, 3);
    }

    $stats = [];
    for ($p = 0; $p < 100; $p++) {
        $stats = $server->getStats();
        if (($stats['totals']['active_connections'] ?? 0) >= CONNS) break;
        usleep(20000);
    }

    $w = array_values($stats['workers']);
    $gauge_sum = array_sum(array_column($w, 'active_connections'));
    $holders   = count(array_filter(array_column($w, 'active_connections')));

    echo 'gauge_total=',  (($stats['totals']['active_connections'] ?? 0) === CONNS ? 1 : 0), "\n";
    echo 'gauge_summed=', (($stats['totals']['active_connections'] ?? -1) === $gauge_sum ? 1 : 0), "\n";
    echo 'gauge_spread_or_darwin=',
         (PHP_OS_FAMILY === 'Darwin' || $holders > 1 ? 1 : 0), "\n";

    foreach ($held as $s) { fclose($s); }

    /* A method the parser refuses: the connection answers 400 and closes, and
     * the worker that served it bumps its own slot. */
    $bad = [];
    for ($i = 0; $i < CONNS; $i++) {
        $s = stream_socket_client("tcp://127.0.0.1:$port", $e, $m, 3);
        stream_set_timeout($s, 3);
        fwrite($s, "!!!BAD / HTTP/1.1\r\nHost: x\r\n\r\n");
        $bad[] = $s;
    }

    $replies = 0;
    foreach ($bad as $s) {
        if (str_starts_with((string)fread($s, 64), 'HTTP/1.1 400')) { $replies++; }
        fclose($s);
    }

    for ($p = 0; $p < 100; $p++) {
        $stats = $server->getStats();
        if (($stats['totals']['parse_errors_400_total'] ?? 0) >= CONNS) break;
        usleep(20000);
    }

    $w = array_values($stats['workers']);
    $err_sum = array_sum(array_column($w, 'parse_errors_400_total'));

    echo 'replies_400=',  ($replies === CONNS ? 1 : 0), "\n";
    echo 'errors_total=', (($stats['totals']['parse_errors_400_total'] ?? 0) === CONNS ? 1 : 0), "\n";
    echo 'errors_summed=',(($stats['totals']['parse_errors_400_total'] ?? -1) === $err_sum ? 1 : 0), "\n";
    echo 'aggregate_matches=',
         (($stats['totals']['parse_errors_4xx_total'] ?? -1)
            === ($stats['totals']['parse_errors_400_total'] ?? -2) ? 1 : 0), "\n";
    echo "done\n";

    $server->stop();
});

$server->start();
--EXPECTF--
gauge_total=1
gauge_summed=1
gauge_spread_or_darwin=1
replies_400=1
errors_total=1
errors_summed=1
aggregate_matches=1
done
%A
