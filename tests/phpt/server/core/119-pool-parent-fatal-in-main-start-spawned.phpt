--TEST--
HttpServer pool: a fatal error on the main coroutine while start() runs in a spawned one ends the process with that one fatal
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
/* #446: the bailout starts outside start(): bailout_all_coroutines() switches
 * into the coroutine that waits in start() to unwind it, and the parent must
 * not suspend again from there. */
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

spawn(function () use ($server) {
    $server->start();
    echo "start returned\n";
});

for ($i = 0; $i < 50 && @file_get_contents("http://127.0.0.1:$port/") !== 'pong'; $i++) {
    delay(100);
}

echo "up\n";
eval('function tas_dup() {}');
eval('function tas_dup() {}');
?>
--EXPECTF--
up

Fatal error: Cannot redeclare function tas_dup() %s
[true-async-server] worker exited
[true-async-server] worker exited
[true-async-server] pool parent: fatal error, 2 worker thread(s) joined
