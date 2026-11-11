--TEST--
HttpServer pool: a fatal error while the workers run a 300 ms bootloader ends the process with that one fatal
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
/* #446: the fatal hits while both worker threads are still in the
 * bootloader, before their start() task runs. A worker whose task is dropped
 * by the closed pool prints nothing, so zero to two "worker exited" lines may
 * come; the parent still joins both threads before it frees what they use. */
use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\delay;
use function Async\spawn;

require __DIR__ . '/../_free_port.inc';

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', tas_free_port())
    ->setWorkers(2)
    ->setShutdownTimeout(2)
    ->setBootloader(static function (): void { usleep(300000); });

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
--EXPECTREGEX--
^booting

Fatal error: Cannot redeclare function tas_dup\(\) [^\n]*
(\[true-async-server\] worker exited
){0,2}\[true-async-server\] pool parent: fatal error, worker threads joined$
