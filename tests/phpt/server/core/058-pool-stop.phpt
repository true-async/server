--TEST--
HttpServer: stop() on the pool parent retires the cohort and blocks until the server is down (#117)
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
/* stop() used to throw on a pool parent. It now publishes STOP on the control
 * channel, wakes every worker (uv_async, not a poll), and suspends until the
 * cohort has drained and the listen sockets are closed — so by the time it
 * returns the port is free and start() has come back. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;

require __DIR__ . '/../_free_port.inc';
$port = tas_free_port();

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(5)
    ->setWriteTimeout(5)
    ->setWorkers(3)
    ->setBootloader(static function (): void {});

$server = new HttpServer($config);
$server->addHttpHandler(function ($req, $res) {
    $res->setStatusCode(200)->setBody('pong');
});

spawn(function () use ($server, $port) {
    $curl = static fn (): string => (string) shell_exec(sprintf(
        'curl -s --max-time 2 http://127.0.0.1:%d/ 2>/dev/null', $port));

    $up = '';
    for ($i = 0; $i < 50 && $up !== 'pong'; $i++) {
        usleep(200000);
        $up = $curl();
    }

    echo "up=", $up, "\n";

    $t0 = microtime(true);
    $ok = $server->stop();          // suspends until the pool is down
    $elapsed = microtime(true) - $t0;

    echo "stop=", var_export($ok, true), "\n";

    /* stop() promises the server is gone, not merely asked to go. */
    echo "running=", var_export($server->isRunning(), true), "\n";
    echo "serving=", var_export($curl() !== 'pong', true), "\n";

    /* The wakeup is an event, not a 1s poll — this must not take seconds. */
    echo "prompt=", var_export($elapsed < 2.0, true), "\n";
});

$server->start();
echo "started_returned=yes\n";
?>
--EXPECTF--
up=pong%A
started_returned=yes%A
stop=true
running=false
serving=true
prompt=true%A
