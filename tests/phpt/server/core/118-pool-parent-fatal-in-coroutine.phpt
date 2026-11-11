--TEST--
HttpServer pool: a fatal error from PHP in a parent coroutine ends the process with that one fatal
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
/* #446: an E_ERROR that is not memory exhaustion, raised while start() runs
 * on the main coroutine. A parent that suspends again in its drain after the
 * bailout segfaults on release and fails an assertion on a debug build. */
use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\delay;
use function Async\spawn;

require __DIR__ . '/../_free_port.inc';
$port = tas_free_port();

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
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
