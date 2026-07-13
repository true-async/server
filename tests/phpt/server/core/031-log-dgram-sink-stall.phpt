--TEST--
Log sink (#121): a wedged unix-datagram collector still lets the server stop, and leaks nothing
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
if (PHP_OS_FAMILY === 'Windows') die('skip udg:// is POSIX-only');
if (!function_exists('_http_log_flood')) {
    die('skip built without --enable-http-server-test-hooks');
}
?>
--FILE--
<?php
/* An AF_UNIX datagram socket is reliable, not lossy: a collector that stops
 * reading fills its receive queue and the sink's sendto() blocks rather than
 * dropping. That write runs on libuv's blocking pool and cannot be cancelled, so
 * it is the transport the stop deadline has to be able to walk away from.
 *
 * Before the deadline existed this sank the whole stop: the drain waited out the
 * stopping thread's 3 s budget, which then abandoned the sink and leaked its io.
 * The queue here is pre-filled, so the sink's very first write is already stuck. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\LogSeverity;
use function Async\spawn;

require_once __DIR__ . '/../_free_port.inc';

$port    = tas_free_port();
$udgPath = sys_get_temp_dir() . '/php-http-031-' . getmypid() . '.sock';
@unlink($udgPath);

/* Bind the collector and never read a single datagram. */
$udg = stream_socket_server("udg://$udgPath", $errno, $errstr, STREAM_SERVER_BIND);
if (!$udg) { echo "FAIL: bind $errstr\n"; exit(1); }

/* Stuff its receive queue full, so the sink cannot get even one record out. */
$stuffer = stream_socket_client("udg://$udgPath", $cerrno, $cerrstr);
if (!$stuffer) { echo "FAIL: connect $cerrstr\n"; exit(1); }
stream_set_blocking($stuffer, false);
$filled = 0;
while (($n = @stream_socket_sendto($stuffer, str_repeat('P', 1024))) > 0) {
    $filled += $n;
}
echo 'queue_wedged=', ($filled > 0 ? 1 : 0), "\n";

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setStatsEnabled(true)
    ->setLogSinks([
        ['type' => 'syslog', 'target' => "udg://$udgPath",
         'facility' => 'daemon', 'level' => LogSeverity::INFO],
    ]);

$server = new HttpServer($config);
$server->addHttpHandler(function ($req, $res) { $res->setBody('x')->end(); });

$t0 = 0.0;
spawn(function () use ($server, &$t0) {
    usleep(50000);
    _http_log_flood($server, 40000);
    $t0 = microtime(true);
    $server->stop();
});

$server->start();
$elapsed = microtime(true) - $t0;

/* The log thread's own deadline has to fire, not the stopping thread's 3 s
 * backstop — that one only ever abandoned the sink and leaked it. */
echo 'stopped=1', "\n";
echo 'bounded=', ($elapsed < 2.5 ? 1 : 0), "\n";

fclose($stuffer);
fclose($udg);
@unlink($udgPath);
echo "Done\n";
?>
--EXPECTF--
queue_wedged=1
%Astopped=1
bounded=1
Done
