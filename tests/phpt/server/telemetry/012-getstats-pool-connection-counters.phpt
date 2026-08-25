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
 * SUM that survives a worker retiring.
 *
 * Which worker accepts is not a promise (#240), and a run where one worker took
 * every connection proves nothing about summing: the sum, the maximum and that
 * worker's own field are the same number then. The gauge half opens rounds until
 * the per-worker column shows two holders, and reports a spread that never
 * happened instead of asserting past it. The parse-error half carries no such
 * guard: its summation is proved only on a run where the six requests did land
 * on more than one worker. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;

require_once __DIR__ . '/../_free_port.inc';

const CONNS = 6;
const SPREAD_ROUNDS = 8;

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
     * and no request is what keeps the handler out of the measurement. Each
     * round adds to the ones already held, so the kernel has more chances to
     * hand one to a second worker. */
    $held = [];
    $stats = [];
    $spread = 'none';
    /* A SO_REUSEPORT set on macOS gives every accept to one socket, so more
     * rounds cannot produce a spread there and one round is all that is run. */
    $rounds = PHP_OS_FAMILY === 'Darwin' ? 1 : SPREAD_ROUNDS;

    for ($round = 0; $round < $rounds; $round++) {
        for ($i = 0; $i < CONNS; $i++) {
            /* A refused connect would sit in $held as false, count against the
             * gauge for the rest of the run and blame the counters for it. */
            $socket = stream_socket_client("tcp://127.0.0.1:$port", $e, $m, 3);
            if ($socket === false) {
                echo "connect failed in round $round: $m\n";
                $server->stop();
                return;
            }

            $held[] = $socket;
        }

        for ($p = 0; $p < 100; $p++) {
            $stats = $server->getStats();
            if (($stats['totals']['active_connections'] ?? 0) >= count($held)) break;
            usleep(20000);
        }

        $holders = array_filter(array_column(array_values($stats['workers']), 'active_connections'));
        if (count($holders) > 1) {
            $spread = 'proven';
            break;
        }
    }

    if ($spread === 'none' && PHP_OS_FAMILY === 'Darwin') {
        $spread = 'darwin-none';
    }

    $w = array_values($stats['workers']);
    $gauge_sum = array_sum(array_column($w, 'active_connections'));

    echo 'gauge_total=',  (($stats['totals']['active_connections'] ?? 0) === count($held) ? 1 : 0), "\n";
    echo 'gauge_summed=', (($stats['totals']['active_connections'] ?? -1) === $gauge_sum ? 1 : 0), "\n";
    echo 'gauge_spread=', $spread, "\n";

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
gauge_spread=%rproven|darwin-none%r
replies_400=1
errors_total=1
errors_summed=1
aggregate_matches=1
done
%A
