--TEST--
HttpServer pool: memory exhausted in one worker leaves the pool serving, and reload() rotates it
--EXTENSIONS--
true_async_server
true_async
--INI--
memory_limit=32M
--FILE--
<?php
/* #439: a coroutine a handler spawned passes memory_limit while the worker
 * serves. The engine keeps that error state until the thread ends, so the
 * thread pool worker that ran the server must not take another task (its
 * scheduler is shutting down) and must not close the pool either: the other
 * worker goes on serving and the next reload() rotates the pool. The wait
 * after reload() is there because a request sent during the rotation can be
 * refused without any fatal at all. */
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
        delay(100);
    }

    $res->setBody('pong');
});

spawn(function () use ($server, $port) {
    for ($i = 0; $i < 50 && @file_get_contents("http://127.0.0.1:$port/") !== 'pong'; $i++) {
        delay(100);
    }

    echo "up\n";
    @file_get_contents("http://127.0.0.1:$port/slow");
    delay(1500);
    echo "reload: ", var_export($server->reload(), true), "\n";

    for ($i = 0; $i < 50 && @file_get_contents("http://127.0.0.1:$port/") !== 'pong'; $i++) {
        delay(100);
    }

    echo $i < 50 ? "served after reload\n" : "not served after reload\n";
    $server->stop();
});

$server->start();
echo "start returned\n";
?>
--EXPECTREGEX--
^(?=.*\n\[true-async-server\] worker ended by a fatal error\n)up

Fatal error: Allowed memory size of 33554432 bytes exhausted [^\n]*
(?:(?:|\[true-async-server\] worker ended by a fatal error|\[true-async-server\] worker exited)\n)*reload: (?:\[true-async-server\] worker exited\n)*true
(?:\[true-async-server\] worker exited\n)*served after reload
(?:\[true-async-server\] worker exited\n)*start returned$
