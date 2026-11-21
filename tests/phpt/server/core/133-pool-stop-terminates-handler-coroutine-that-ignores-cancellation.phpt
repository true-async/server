--TEST--
HttpServer: stop() returns although a handler left a coroutine that catches its cancellation
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
exec('curl --version 2>&1', $out, $rc);
if ($rc !== 0) die('skip curl CLI not available');
?>
--INI--
async.shutdown_grace=300
--FILE--
<?php

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;

require __DIR__ . '/../_free_port.inc';
$port = tas_free_port();

/* A retry loop that catches \Throwable, and with it every cancellation. */
$stubborn = static function (): void {
    while (true) {
        try {
            \Async\delay(100);
        } catch (\Throwable $e) {
        }
    }
};

$t0 = microtime(true);

/* Cancellation may unwind the main coroutine: only a shutdown function is sure to report. */
register_shutdown_function(static function () use (&$t0): void {
    echo "exited in time: ", var_export(microtime(true) - $t0 < 10.0, true), "\n";
});

$curl = static fn (): string => (string) shell_exec(sprintf('curl -s --max-time 2 http://127.0.0.1:%d/', $port));

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(5)
    ->setWriteTimeout(5)
    ->setWorkers(2)
    ->setShutdownTimeout(1);

$server = new HttpServer($config);
$server->addHttpHandler(function ($req, $res) use ($stubborn) {
    spawn($stubborn);
    $res->setStatusCode(200)->setBody('pong');
});

spawn(function () use ($server, $curl, &$t0) {
    $up = '';
    for ($i = 0; $i < 50 && $up !== 'pong'; $i++) {
        usleep(200000);
        $up = $curl();
    }

    echo "up=", $up, "\n";
    $t0 = microtime(true);
    $server->stop();
});

$server->start();
echo "start returned\n";
?>
--EXPECTF--
up=pong
%Astart returned
%Aexited in time: true%A
