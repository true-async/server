--TEST--
HttpServer pool: a bailout while reload() submits the fresh cohort leaves no completion on freed state
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
if (!function_exists('_http_fault_enable')) die('skip fault injection required');
?>
--FILE--
<?php
/* #446: the rotation has finished and reload() hands the replacement threads
 * their start() tasks; a bailout after the first one leaves that task's
 * future, which the fresh worker resolves when it stops, and must not leave it
 * pointing at the wait state start() frees in its teardown. */
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

spawn(function () use ($server, $port) {
    for ($i = 0; $i < 50 && @file_get_contents("http://127.0.0.1:$port/") !== 'pong'; $i++) {
        delay(100);
    }

    echo "up\n";
    _http_fault_enable('server/reload/submit_bailout', true);
    $server->reload();
    echo "reload returned\n";
});

$server->start();
echo "start returned\n";
?>
--EXPECT--
up
[true-async-server] worker exited
[true-async-server] worker exited
[true-async-server] worker exited
[true-async-server] pool parent: fatal error, worker threads joined
