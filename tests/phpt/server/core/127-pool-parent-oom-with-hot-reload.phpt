--TEST--
HttpServer pool: memory exhausted with hot reload armed closes the file watcher without calling PHP
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
/* #446: under exhausted memory the watcher's close() cannot be called, and
 * the watch coroutine is unwound inside its iterator. It closes the watch in
 * C and drops the iterator and the watcher, or their loop handles fail the
 * debug assertion that the loop is stopped. php-async warns about the watch
 * coroutine its shutdown cancelled and queued after the bailout had unwound
 * it; it does so for any coroutine waiting at a memory fatal, with or without
 * this server, so the line is optional here. */
use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\delay;
use function Async\spawn;

require __DIR__ . '/../_free_port.inc';
$port = tas_free_port();

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->enableHotReload([sys_get_temp_dir()])
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
--EXPECTREGEX--
^up
in flight

Fatal error: Allowed memory size of 33554432 bytes exhausted [^\n]*
\[true-async-server\] worker exited
\[true-async-server\] worker exited
\[true-async-server\] pool parent: fatal error, 2 worker thread\(s\) joined(

Warning: 1 deferred coroutines were not executed in Unknown on line 0)?$
