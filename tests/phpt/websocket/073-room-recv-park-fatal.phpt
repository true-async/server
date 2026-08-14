--TEST--
Rooms: a fatal error on a thread with a parked recv() leaves no live loop handle behind
--SKIPIF--
<?php
if (!PHP_ZTS) die('skip ZTS required');
if (!class_exists('Async\ThreadPool')) die('skip ThreadPool not available');
?>
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
/* The park holds the thread's reactor open, and a fatal error longjmps out of
 * the suspend. Released only on the normal path, the ref outlives the worker:
 * the scheduler reaches its teardown with a live handle and aborts the whole
 * process on "The event loop must be stopped". Here the worker must die alone,
 * and the main thread must live to say so. */
use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use Async\ThreadPool;
use function Async\spawn;
use function Async\await;
use function Async\delay;

$server = new HttpServer(
    (new HttpServerConfig())->addListener('127.0.0.1', 18073)
);
$server->enableRooms();

$room = $server->room('run/73/control');
$ack  = $server->room('test/ack73');

spawn(function () use ($server, $room, $ack) {
    $ack->subscribe();

    $pool = new ThreadPool(1);

    $task = $pool->submit(function () use ($room, $ack) {
        $room->subscribe();

        spawn(fn() => $room->recv());

        $ack->publish('parked');
        delay(50);

        ini_set('memory_limit', '2M');
        $eat = str_repeat('x', 64 * 1024 * 1024);

        return 'not reached' . strlen($eat);
    });

    echo 'ack: ', $ack->recv(3000), "\n";

    try {
        $outcome = var_export(await($task), true);
    } catch (\Throwable $e) {
        $outcome = 'threw ' . $e::class;
    }

    $ack->unsubscribe();

    echo 'task: ', $outcome, "\n";
    echo "main: alive\n";
});
?>
--EXPECTF--
ack: parked
%Atask: threw Async\ThreadTransferException
main: alive
