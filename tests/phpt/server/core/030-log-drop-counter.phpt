--TEST--
Log sink (#121): a stalled receiver costs records, is drop-counted, and still lets the server stop
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
if (PHP_OS_FAMILY === 'Windows') die('skip stream_socket_pair(STREAM_PF_UNIX) is POSIX-only');
if (!function_exists('_http_log_flood')) {
    die('skip built without --enable-http-server-test-hooks');
}
?>
--FILE--
<?php
/* A sink's ring is bounded on purpose: the producer must never block, so a
 * receiver that stops reading costs records rather than throughput. What is NOT
 * acceptable is losing them quietly — an operator reading getStats has to see
 * that the log is incomplete, or they will trust a truncated audit trail.
 *
 * The sink here writes into one end of a socket pair that nobody ever reads:
 * the kernel buffer fills, the write in flight stops completing, the ring fills
 * behind it, and every further record is dropped. Stopping the server must still
 * work — the log thread cannot wait out a write that will never complete, or it
 * never leaves its loop and the process aborts on the loop-alive assert (#121). */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\LogSeverity;
use function Async\spawn;

require_once __DIR__ . '/../_free_port.inc';

$port = tas_free_port();

/* Deliberately keep $reader alive but never read from it. */
[$reader, $writer] = stream_socket_pair(STREAM_PF_UNIX, STREAM_SOCK_STREAM, 0);

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setStatsEnabled(true)
    ->setLogSinks([
        ['type' => 'stream', 'stream' => $writer, 'format' => 'plain',
         'level' => LogSeverity::INFO],
    ]);

$server = new HttpServer($config);
$server->addHttpHandler(function ($req, $res) { $res->setBody('x')->end(); });

spawn(function () use ($server) {
    usleep(50000);

    $before = $server->getStats()['totals']['log_records_dropped_total'];

    /* ~90 bytes per record: far past the socket buffer plus the 64 KiB ring. */
    $reported = _http_log_flood($server, 40000);
    $after    = $server->getStats()['totals']['log_records_dropped_total'];

    echo 'before=', $before, "\n";
    echo 'dropped=', ($after > 0 ? 1 : 0), "\n";
    echo 'counter_matches_hook=', ($after === $reported ? 1 : 0), "\n";
    /* Not everything is lost: what fitted in the ring and the socket got out. */
    echo 'not_all_dropped=', ($after < 40000 ? 1 : 0), "\n";

    $server->stop();
});

$server->start();
echo "stopped\n";
?>
--EXPECTF--
%Ahttp_server log sink failed: ring overflow, dropped=%d
before=0
dropped=1
counter_matches_hook=1
not_all_dropped=1
stopped
