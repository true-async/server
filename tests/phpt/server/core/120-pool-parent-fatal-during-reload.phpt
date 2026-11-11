--TEST--
HttpServer pool: a fatal error while reload() waits for the old workers ends the process with that one fatal
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
/* #446, #439 item 3: reload() waits in the pool rotation while one old
 * worker finishes a 1500 ms request; a fatal error in another parent coroutine
 * unwinds reload() through that wait. The idle old worker has exited before the
 * fatal; the busy one exits after it, and the parent waits for it. */
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
        delay(1500);
    }

    $res->setBody('pong');
});

spawn(function () use ($server, $port) {
    for ($i = 0; $i < 50 && @file_get_contents("http://127.0.0.1:$port/") !== 'pong'; $i++) {
        delay(100);
    }

    echo "up\n";
    spawn(fn () => @file_get_contents("http://127.0.0.1:$port/slow"));
    delay(100);
    spawn(function () use ($server) {
        echo "reloading\n";
        $server->reload();
        echo "reload returned\n";
    });
    delay(300);
    eval('function tas_dup() {}');
    eval('function tas_dup() {}');
});

$server->start();
echo "start returned\n";
?>
--EXPECTF--
up
reloading
[true-async-server] worker exited

Fatal error: Cannot redeclare function tas_dup() %s
[true-async-server] worker exited
[true-async-server] pool parent: fatal error, worker threads joined
