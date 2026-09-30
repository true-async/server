--TEST--
HttpServer: a stop() that runs while start() is still starting stops the server
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\LogSeverity;
use function Async\spawn;
use function Async\delay;

require_once __DIR__ . '/../_free_port.inc';

/* start() connects to a TCP log sink before it marks the server running, and
 * the connect suspends it; that is the window the stop() below lands in. The
 * sink's port is the one the kernel gives its listener, and the server's is
 * probed apart from it: Windows hands ephemeral ports out in order, so the
 * port after one just taken is the next another test gets. */
$listener = stream_socket_server("tcp://127.0.0.1:0", $errno, $errstr);
if (!$listener) { echo "FAIL: listener $errstr\n"; exit(1); }
$sysName  = stream_socket_get_name($listener, false);
$sysPort  = (int) substr($sysName, strrpos($sysName, ':') + 1);
$httpPort = tas_free_port();

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $httpPort)
    ->setLogSinks([
        ['type' => 'syslog', 'target' => "tcp://127.0.0.1:$sysPort",
         'facility' => 'local0', 'level' => LogSeverity::INFO],
    ]);

$server = new HttpServer($config);
$server->addHttpHandler(function ($req, $res) { $res->setStatusCode(200)->setBody('OK')->end(); });

spawn(function () use ($server) {
    echo "stop while running: ", var_export($server->isRunning(), true), "\n";
    $server->stop();
});

/* A lost stop() leaves start() waiting for good; this unwinds it so the test
 * fails on its output instead of on the run-tests timeout. */
$watchdog = spawn(function () use ($server) {
    delay(3000);
    echo "FAIL: start() still running 3 s after stop()\n";
    $server->stop();
});

$server->start();
$watchdog->cancel();

echo "start returned, running: ", var_export($server->isRunning(), true), "\n";
fclose($listener);
?>
--EXPECT--
stop while running: false
start returned, running: false
