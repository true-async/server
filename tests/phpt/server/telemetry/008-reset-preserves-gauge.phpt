--TEST--
resetTelemetry (#5): reset keeps live occupancy gauges (no close-time underflow)
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
if (PHP_OS_FAMILY === 'Windows') die('skip libuv on Windows lacks SO_REUSEPORT');
?>
--FILE--
<?php
/* resetTelemetry() clears the monotonic totals but must preserve the live
 * gauges (conns_active_*, active_requests). Zeroing conns_active_h1 while a
 * keep-alive connection is still open makes the close-time decrement underflow
 * the uint64 gauge. After reset the open connection must still read 1, and once
 * it closes the gauge must settle back to 0 — never a huge wrapped value. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;

require_once __DIR__ . '/../_free_port.inc';

$port = tas_free_port();
$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(10)
    ->setWriteTimeout(10)
    ->setStatsEnabled(true);

$server = new HttpServer($config);
$server->addHttpHandler(fn($req, $res) => $res->setStatusCode(200)->setBody('ok')->end());

$h1 = fn() => $server->getStats()['totals']['conns_active_h1'];

spawn(function () use ($server, $port, $h1) {
    usleep(100000);

    /* Keep-alive connection: no Connection: close, socket stays open. */
    $c = @stream_socket_client("tcp://127.0.0.1:$port", $e1, $e2, 2);
    fwrite($c, "GET /a HTTP/1.1\r\nHost: x\r\n\r\n");
    stream_set_timeout($c, 2);
    // read just the response head so the connection stays open
    $buf = '';
    while (!str_contains($buf, "\r\n\r\n") && strlen($buf) < 8192) {
        $chunk = fread($c, 512);
        if ($chunk === '' || $chunk === false) break;
        $buf .= $chunk;
    }

    for ($i = 0; $i < 50 && $h1() < 1; $i++) usleep(20000);
    echo 'open_before_reset=', ($h1() === 1 ? 1 : 0), "\n";

    $server->resetTelemetry();
    echo 'total_after_reset=', $server->getStats()['totals']['total_requests'], "\n";
    echo 'gauge_after_reset=', ($h1() === 1 ? 1 : 0), "\n";   // preserved, not zeroed

    fclose($c);   // now the decrement fires

    $v = 1;
    for ($i = 0; $i < 50; $i++) { $v = $h1(); if ($v === 0) break; usleep(20000); }
    echo 'gauge_after_close=', ($v === 0 ? 1 : 0), "\n";       // 0, not underflowed
    echo "done\n";

    $server->stop();
});
$server->start();
?>
--EXPECTF--
open_before_reset=1
total_after_reset=0
gauge_after_reset=1
gauge_after_close=1
done%A
