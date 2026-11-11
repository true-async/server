--TEST--
HttpServer pool: memory exhausted in a parent coroutine ends the process with one fatal after the workers are joined
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
exec('curl --version 2>&1', $out, $rc);
if ($rc !== 0) die('skip curl CLI not available');
?>
--INI--
memory_limit=32M
--FILE--
<?php
/* #446: after a fatal error in a parent coroutine the pool parent must not
 * suspend in its drain or call Async\protect, which the engine refuses once
 * memory is exhausted: that gives a second fatal and an EBUSY uv_loop_close().
 * A request is in flight on a worker for 600 ms when the fatal hits, so a
 * parent that does not wait for its workers exits before their "worker exited"
 * lines and the join line. The slow request comes from a curl process: a
 * parent coroutine waiting on it could not be unwound once memory is
 * exhausted, and the engine would warn about it. */
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
$server->addHttpHandler(function ($req, $res) {
    if ($req->getPath() === '/slow') {
        delay(600);
    }

    $res->setBody('pong');
});

spawn(function () use ($port) {
    for ($i = 0; $i < 50 && @file_get_contents("http://127.0.0.1:$port/") !== 'pong'; $i++) {
        delay(100);
    }

    echo "up\n";
    $curl = "curl -s --max-time 5 http://127.0.0.1:$port/slow";
    if (PHP_OS_FAMILY === 'Windows') {
        pclose(popen("start /B $curl >NUL 2>&1", 'r'));
    } else {
        exec("$curl >/dev/null 2>&1 &");
    }
    delay(100);
    echo "in flight\n";
    $s = str_repeat('x', 64 * 1024 * 1024);
});

$server->start();
echo "start returned\n";
?>
--EXPECTF--
up
in flight

Fatal error: Allowed memory size of 33554432 bytes exhausted %s in %s on line %d
[true-async-server] worker exited
[true-async-server] worker exited
[true-async-server] pool parent: fatal error, 2 worker thread(s) joined
