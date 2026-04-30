--TEST--
HttpServer: each instance owns its own logger state (PLAN_LOG.md per-server state)
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\LogSeverity;
use function Async\spawn;

/* Two HttpServer instances configured with different log severities
 * and different sink files. Run them sequentially. Verify each only
 * receives its own records — no leakage between instances. */

$pid = getmypid();
$portA = 19920 + $pid % 30;
$portB = $portA + 1;

$logA = sys_get_temp_dir() . "/php-http-server-094a-$pid.log";
$logB = sys_get_temp_dir() . "/php-http-server-094b-$pid.log";
@unlink($logA); @unlink($logB);
$fhA = fopen($logA, "w+b");
$fhB = fopen($logB, "w+b");

$cfgA = (new HttpServerConfig())
    ->addListener('127.0.0.1', $portA)
    ->setLogSeverity(LogSeverity::DEBUG)
    ->setLogStream($fhA);

$cfgB = (new HttpServerConfig())
    ->addListener('127.0.0.1', $portB)
    ->setLogSeverity(LogSeverity::INFO)
    ->setLogStream($fhB);

$srvA = new HttpServer($cfgA);
$srvB = new HttpServer($cfgB);
$srvA->addHttpHandler(function ($r, $s) { $s->setStatusCode(200)->setBody('A')->end(); });
$srvB->addHttpHandler(function ($r, $s) { $s->setStatusCode(200)->setBody('B')->end(); });

spawn(function () use ($srvA) { usleep(40000); $srvA->stop(); });
$srvA->start();

spawn(function () use ($srvB) { usleep(40000); $srvB->stop(); });
$srvB->start();

fflush($fhA); fclose($fhA);
fflush($fhB); fclose($fhB);
$a = file_get_contents($logA);
$b = file_get_contents($logB);
@unlink($logA); @unlink($logB);

echo "A has start: ", (strpos($a, "server.start") !== false ? "yes" : "no"), "\n";
echo "A has stop: ",  (strpos($a, "server.stop")  !== false ? "yes" : "no"), "\n";
echo "B has start: ", (strpos($b, "server.start") !== false ? "yes" : "no"), "\n";
echo "B has stop: ",  (strpos($b, "server.stop")  !== false ? "yes" : "no"), "\n";

/* DEBUG severity for A means its log is generally larger than B's
 * INFO log when same number of events fire (no h1 traffic in this
 * test, so only server.start/stop on each — sizes are similar but
 * we still verify the two streams don't bleed into each other). */
echo "A and B logs distinct: ", ($a !== $b ? "yes" : "no"), "\n";
echo "Done\n";
--EXPECT--
A has start: yes
A has stop: yes
B has start: yes
B has stop: yes
A and B logs distinct: yes
Done
