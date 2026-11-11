--TEST--
HttpServer pool: a fatal error with reload on SIGHUP armed stops the signal event before exit
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
if (PHP_OS_FAMILY === 'Windows') die('skip no SIGHUP on Windows');
?>
--FILE--
<?php
/* #446: the SIGHUP orchestrator waits on a started signal event. Unwound by
 * the bailout without stopping it, the event keeps the loop alive, and a debug
 * build fails the assertion that the loop is stopped at exit. */
use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\delay;
use function Async\spawn;

require __DIR__ . '/../_free_port.inc';
$port = tas_free_port();

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->enableReloadOnSignal()
    ->setWorkers(2)
    ->setShutdownTimeout(2)
    ->setBootloader(static function (): void {});

$server = new HttpServer($config);
$server->addHttpHandler(function ($req, $res) { $res->setBody('pong'); });

spawn(function () use ($port) {
    for ($i = 0; $i < 50 && @file_get_contents("http://127.0.0.1:$port/") !== 'pong'; $i++) {
        delay(100);
    }

    echo "up\n";
    eval('function tas_dup() {}');
    eval('function tas_dup() {}');
});

$server->start();
echo "start returned\n";
?>
--EXPECTF--
up

Fatal error: Cannot redeclare function tas_dup() %s
[true-async-server] worker exited
[true-async-server] worker exited
[true-async-server] pool parent: fatal error, worker threads joined
