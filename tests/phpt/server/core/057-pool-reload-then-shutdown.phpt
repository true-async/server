--TEST--
HttpServer: reload() then graceful_shutdown() — rotated + final cohorts both reaped, clean exit (#93)
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
/* Rotate the pool once (reload disposes the retired cohort's completion
 * futures), then graceful_shutdown() (the final cohort's futures are disposed
 * on the way out). Exercises both disposal sites together — a leak or a
 * cross-thread race on either would trip the loop-alive assert / abort. */

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

    $server->reload();
    echo "reloaded\n";

    for ($i = 0; $i < 50 && $curl() !== 'pong'; $i++) {
        usleep(200000);
    }

    \Async\graceful_shutdown();
});

$server->start();
echo "stopped=clean\n";
?>
--EXPECTF--
up=pong%A
reloaded%A
stopped=clean%A
