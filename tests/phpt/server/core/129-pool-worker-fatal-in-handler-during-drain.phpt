--TEST--
HttpServer pool: a fatal error in a handler while the worker drains on stop() leaves no libuv handle behind
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
/* The handler's bailout firewall answers the request and the worker goes on
 * draining. The drain's grace window is an Async\Timeout created before the
 * fatal, and a fatal error marks every live object destructed: the Timeout
 * closed its libuv timer only in its destructor, so the worker's loop closed
 * with EBUSY and a leftover timer. */
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
        delay(800);
        eval('function tas_dup() {}');
        eval('function tas_dup() {}');
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
--EXPECTF--
up
stopping
[true-async-server] worker exited

Fatal error: Cannot redeclare function tas_dup() %s

Warning: [true-async-server] zend_bailout in h1 handler: %s
%A[true-async-server] --- end C backtrace ---
[true-async-server] worker exited
start returned
