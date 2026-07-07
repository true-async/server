--TEST--
HttpServer: setShutdownTimeout(0) — graceful_shutdown() quiesces the pool immediately, no drain wait, clean exit (#93)
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
/* shutdown_timeout == 0 means "do not wait for workers to drain". The parent
 * skips the bounded drain loop and quiesces the per-worker completion futures
 * straight away: resolved ones are disposed, any still-armed one has its
 * trigger stopped (unref, handle left open so a late cross-thread signal is a
 * safe no-op). Either way the loop goes quiescent and start() returns without
 * tripping the scheduler loop-alive assert. Workers also run a persistent
 * background coroutine to prove graceful_shutdown cancels it rather than
 * wedging teardown. */

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
    ->setShutdownTimeout(0)
    ->setBootloader(static function (): void {
        \Async\spawn(static function (): void {
            while (true) {
                \Async\delay(200);
            }
        });
    });

$server = new HttpServer($config);
$server->addHttpHandler(function ($req, $res) {
    $res->setStatusCode(200)->setBody('pong');
});

spawn(function () use ($port) {
    $curl = static fn (): string => (string) shell_exec(sprintf(
        'curl -s --max-time 2 http://127.0.0.1:%d/ 2>/dev/null', $port));

    $up = '';
    for ($i = 0; $i < 50 && $up !== 'pong'; $i++) {
        usleep(200000);
        $up = $curl();
    }

    echo "up=", $up, "\n";

    \Async\graceful_shutdown();
});

$server->start();
echo "stopped=clean\n";
?>
--EXPECTF--
up=pong%A
stopped=clean%A
