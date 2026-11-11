--TEST--
HttpServer pool: a bailout after the worker threads start and before the pool runs closes the pool and joins them
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
if (!function_exists('_http_fault_enable')) die('skip fault injection required');
?>
--FILE--
<?php
/* #446: the threads exist from the pool's creation and wait on its task
 * channel. A bailout in the setup that follows, before any task is submitted,
 * must close that channel and wait for them: otherwise module shutdown waits
 * for threads that never leave. */
use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;

require __DIR__ . '/../_free_port.inc';

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', tas_free_port())
    ->setWorkers(2)
    ->setShutdownTimeout(2)
    ->setBootloader(static function (): void {});

$server = new HttpServer($config);
$server->addHttpHandler(function ($req, $res) { $res->setBody('pong'); });
_http_fault_enable('server/pool/setup_bailout', true);
$server->start();
echo "start returned\n";
?>
--EXPECT--
[true-async-server] pool parent: fatal error, worker threads joined
