--TEST--
HttpServer pool: after a fatal error the parent waits for its worker threads only up to a bound, then exits
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
/* #446: a bootloader that does not return keeps its thread from ever seeing
 * STOP. The parent waits for setShutdownTimeout() (1 s) plus 10 s, reporting
 * every 5 s, then ends the process instead of hanging. */
use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\delay;
use function Async\spawn;

require __DIR__ . '/../_free_port.inc';

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', tas_free_port())
    ->setWorkers(2)
    ->setShutdownTimeout(1)
    ->setBootloader(static function (): void { sleep(60); });

$server = new HttpServer($config);
$server->addHttpHandler(function ($req, $res) { $res->setBody('pong'); });

spawn(function () {
    delay(100);
    echo "booting\n";
    eval('function tas_dup() {}');
    eval('function tas_dup() {}');
});

$server->start();
echo "start returned\n";
?>
--EXPECTF--
booting

Fatal error: Cannot redeclare function tas_dup() %s
[true-async-server] pool parent: fatal error, waiting for 2 worker thread(s)
[true-async-server] pool parent: fatal error, waiting for 2 worker thread(s)
[true-async-server] pool parent: fatal error, 2 worker thread(s) still running after 11000 ms, exiting
