--TEST--
HttpServer pool: a fatal error in a worker coroutine while the worker drains on stop() ends that worker with one fatal
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
/* #439 items 1 and 2 without memory exhaustion: a fatal error in a coroutine
 * the handler spawned, outside the request firewall, while the worker drains
 * its scope on stop(). The bailout leaves the worker's start() parked in the
 * drain's wait; the teardown after it must neither call PHP nor suspend, and
 * the grace timer that wait armed must not outlive the worker's loop. */
use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\delay;
use function Async\spawn;

require __DIR__ . '/../_free_port.inc';
$port = tas_free_port();

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setWorkers(2)
    ->setShutdownTimeout(5)
    ->setBootloader(static function (): void {});

$server = new HttpServer($config);
$server->addHttpHandler(function ($req, $res) {
    if ($req->getPath() === '/slow') {
        spawn(function () {
            delay(800);
            eval('function tas_dup() {}');
            eval('function tas_dup() {}');
        });
        delay(2000);
    }

    $res->setBody('pong');
});

spawn(function () use ($server, $port) {
    for ($i = 0; $i < 50 && @file_get_contents("http://127.0.0.1:$port/") !== 'pong'; $i++) {
        delay(100);
    }

    echo "up\n";
    spawn(fn () => @file_get_contents("http://127.0.0.1:$port/slow"));
    delay(200);
    echo "stopping\n";
    $server->stop();
});

$server->start();
echo "start returned\n";
?>
--EXPECTREGEX--
^up
stopping
\[true-async-server\] worker exited

Fatal error: Cannot redeclare function tas_dup\(\) [^\n]*
(?:(?:|\[true-async-server\] worker did not start: TrueAsync\\HttpServerRuntimeException: Worker server start bailed out)\n)*start returned(?:\n(?:|\[true-async-server\] worker did not start: TrueAsync\\HttpServerRuntimeException: Worker server start bailed out))*$
