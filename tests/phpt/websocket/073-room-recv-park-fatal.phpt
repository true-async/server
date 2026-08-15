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

        /* Not memory_limit: USE_ZEND_ALLOC=0 replaces the allocator that enforces
         * it, and the valgrind lane would measure a worker that never died. */
        error_reporting(E_ALL & ~E_DEPRECATED);
        trigger_error('the worker dies with a receiver parked', E_USER_ERROR);

        return 'not reached';
    });

    /* Received first, and written as ONE string: the doomed worker writes its
     * fatal to the same stream from another thread, and an echo that suspends
     * between its arguments — or writes them one by one — lets that text land
     * inside this line. Which side of the line it lands on is not ours to order,
     * which is what the leading %A in the expectation is for. */
    $parked = $ack->recv(3000);
    echo "ack: $parked\n";

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
%Aack: parked
%Atask: threw Async\ThreadTransferException
main: alive
