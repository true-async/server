--TEST--
HttpServer: graceful_shutdown() stops a single-worker server (#146)
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
if (PHP_OS_FAMILY === 'Windows') die('skip libuv on Windows lacks SO_REUSEPORT');
?>
--FILE--
<?php
/* setWorkers(1) takes the standalone path, where only stop() closes the
 * listeners. Left armed after cancellation woke start(), one live handle keeps
 * uv_run from returning: the process hangs until run-tests times out. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;

require __DIR__ . '/../_free_port.inc';

/* Cancellation unwinds the main coroutine, so on any build nothing after
 * start() runs: only a shutdown function can report the clean exit. */
register_shutdown_function(static function (): void {
    echo "exited=clean\n";
});

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', tas_free_port())
    ->setWorkers(1);

$server = new HttpServer($config);
$server->addHttpHandler(static function ($req, $res) {
    $res->setStatusCode(200)->setBody('pong');
});

spawn(static function (): void {
    Async\delay(300);
    echo "shutting down\n";
    Async\graceful_shutdown();
});

$server->start();
?>
--EXPECTF--
shutting down%A
exited=clean%A
