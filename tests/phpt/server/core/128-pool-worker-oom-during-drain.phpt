--TEST--
HttpServer pool: memory exhausted in a worker coroutine while the worker drains on stop() ends that worker with one fatal
--EXTENSIONS--
true_async_server
true_async
--INI--
memory_limit=32M
--FILE--
<?php
/* #439 items 1 and 2: stop() reaches the worker while a handler is parked; the
 * worker drains its scope with a grace window. A coroutine the handler spawned
 * exhausts memory inside that window, which the handler's bailout firewall does
 * not cover, so the bailout leaves the worker's start(). The teardown after it
 * may not call PHP: a Scope method called there under exhausted memory raises a
 * second fatal, which skips the rest of the teardown and the task's completion,
 * and the parent then waits for that worker forever. The handler the
 * stop() cancels is the next coroutine to run PHP, so the delayed memory error
 * is raised in it and its firewall logs it: that warning is allowed, in any
 * order with the parent's lines, and nothing else is. */
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
            $a = [];
            for (;;) {
                $a[] = str_repeat('x', 1024);
            }
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

Fatal error: Allowed memory size of 33554432 bytes exhausted [^\n]*
(?:(?:|Warning: \[true-async-server\] zend_bailout in h1 handler: [^\n]*Allowed memory size[^\n]*|\[true-async-server\] --- (?:end )?C backtrace[^\n]*|\/[^\n]*\]|\[true-async-server\] worker did not start: TrueAsync\\HttpServerRuntimeException: Worker server start bailed out)\n)*start returned(?:\n(?:|\[true-async-server\] --- (?:end )?C backtrace[^\n]*|\/[^\n]*\]|\[true-async-server\] worker did not start: TrueAsync\\HttpServerRuntimeException: Worker server start bailed out))*$
