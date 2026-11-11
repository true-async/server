--TEST--
HttpServer pool: a fatal error with a parent log sink stops the sink without waiting for it
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
/* #446: stopping a log sink normally waits for the log thread's final
 * drain, a suspend the parent may not make after a bailout. The sink is handed
 * over without the wait, so its lines may land after the join line. */
use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\delay;
use function Async\spawn;

require __DIR__ . '/../_free_port.inc';
$port = tas_free_port();

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setLogSeverity(\TrueAsync\LogSeverity::INFO)
    ->setLogStream(STDERR)
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
--EXPECTREGEX--
^up

Fatal error: Cannot redeclare function tas_dup\(\) [^\n]*
\[true-async-server\] worker exited
\[true-async-server\] worker exited
\[true-async-server\] pool parent: fatal error, 2 worker thread\(s\) joined
(\d{4}-\d\d-\d\dT[\d:.]+Z INFO server\.[a-z]+ mode=pool[^\n]*\n?)*$
