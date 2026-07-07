--TEST--
HttpServer: graceful_shutdown() drains the worker pool and start() returns cleanly — no loop-alive assert (#93)
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
/* A pool-parent HttpServer awaits worker completion. On Async\graceful_shutdown()
 * the workers drain and exit; the parent's bounded shutdown drain reaps their
 * per-worker completion futures (whose cross-thread triggers would otherwise
 * linger armed on the parent reactor). start() then returns and the process
 * exits cleanly — on a debug build a leaked trigger would trip the scheduler's
 * loop-alive assert (scheduler.c) and abort before "stopped=clean" prints. */

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
