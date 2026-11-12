--TEST--
HttpServer pool: memory exhausted in a worker coroutine while the worker waits for stop() ends that worker with one fatal
--EXTENSIONS--
true_async_server
true_async
--INI--
memory_limit=32M
--FILE--
<?php
/* #439: the same memory exhaustion as core/128, but before stop(): the
 * worker's start() is parked in its wait for stop(), not in the drain, and the
 * handler in delay(). The bailout unwinds the start() coroutine first, and its
 * teardown disposes the server scope, which cancels the parked handler and
 * queues it; the unwinding must take it off the queue before it unwinds it,
 * or the handler runs a second time on a freed context (SIGSEGV). */
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
    delay(1600);
    echo "stopping\n";
    $server->stop();
});

$server->start();
echo "start returned\n";
?>
--EXPECTREGEX--
^(?=.*\n\[true-async-server\] worker ended by a fatal error\n)up
(?:\[true-async-server\] worker exited\n)?
?Fatal error: Allowed memory size of 33554432 bytes exhausted [^\n]*
(?:(?:|Warning: \[true-async-server\] zend_bailout in h1 handler: [^\n]*Allowed memory size[^\n]*|\[true-async-server\] --- (?:end )?C backtrace[^\n]*|\/[^\n]*\]|\[true-async-server\] worker ended by a fatal error|\[true-async-server\] worker exited|stopping)\n)*start returned(?:\n(?:|\[true-async-server\] --- (?:end )?C backtrace[^\n]*|\/[^\n]*\]|\[true-async-server\] worker ended by a fatal error))*$
