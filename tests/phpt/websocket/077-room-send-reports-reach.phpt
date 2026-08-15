--TEST--
Rooms: a reliable send counts every target it reached, and refuses when it reached none
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
use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\RoomDeliveryException;
use Async\ThreadPool;
use function Async\spawn;
use function Async\await;

/* What a reliable send used to be unable to say:
 *   - it reached nobody because nothing is attached to the hub, and no later
 *     attach can rescue this message;
 *   - it reached nobody because the workers are there and the room is empty —
 *     the same zero, a different remedy, so a different message;
 *   - it served the subscribers sitting on the CALLING thread, which used to be
 *     counted as 0 targets reached.
 * The refusals are told apart by their text, so neither can be satisfied by the
 * no-coroutine refusal that throws the same exception class. */
$server = new HttpServer(
    (new HttpServerConfig())->addListener('127.0.0.1', 18077)
);
$server->enableRooms();

$room = $server->room('run/9/control');
$ack  = $server->room('run/9/ack');

spawn(function () use ($server, $room, $ack) {
    try {
        $room->send('stop', 200);
        echo "no worker: NOT refused\n";
    } catch (RoomDeliveryException $e) {
        echo 'no worker: ',
             str_contains($e->getMessage(), 'no thread is attached') ? 'refused' : 'WRONG REASON',
             ', delivered=', $e->delivered, "\n";
    }

    /* subscribe() attaches this thread, so from here on the hub has a worker. */
    $room->subscribe();
    $ack->subscribe();

    echo 'local send delivered: ', $room->send('stop', 200), "\n";
    echo 'and the message is here: ', $room->recv(0) ?? '(lost)', "\n";

    $audit = $server->room('run/9/audit');

    try {
        $audit->send('note', 200);
        echo "empty room: NOT refused\n";
    } catch (RoomDeliveryException $e) {
        echo 'empty room: ',
             str_contains($e->getMessage(), 'nobody has subscribed') ? 'refused' : 'WRONG REASON', "\n";
    }

    $p = $audit->publish('note');
    echo 'empty room publish: served=', $p['served'], ' workers=', $p['workers'], "\n";

    /* One subscriber here, one in another thread: two targets, one number. */
    $pool = new ThreadPool(1);

    $task = $pool->submit(function () use ($room, $ack) {
        $room->subscribe();
        $ack->send('subscribed', 2000);

        $got = $room->recv(3000) ?? '(timeout)';
        $room->unsubscribe();

        return $got;
    });

    echo 'ack: ', $ack->recv(3000), "\n";
    echo 'local+remote delivered: ', $room->send('stop-all', 2000), "\n";
    echo 'here: ', $room->recv(1000) ?? '(lost)', "\n";
    echo 'there: ', await($task), "\n";

    $pool->close();
    $room->unsubscribe();
    $ack->unsubscribe();
});
?>
--EXPECT--
no worker: refused, delivered=0
local send delivered: 1
and the message is here: stop
empty room: refused
empty room publish: served=0 workers=1
ack: subscribed
local+remote delivered: 2
here: stop-all
there: stop-all
