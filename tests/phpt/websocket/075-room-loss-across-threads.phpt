--TEST--
Rooms: a ring that overflows in another thread says so, and keeps the newest 64
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
/* Loss on the receiving side is not the same event as a full mailbox: `dropped`
 * means the transport gave up on a worker, `ws_sub_overflow` means a receiver
 * was too slow for its own ring. A control room needs to tell those apart —
 * one asks for rate limiting, the other says a command was thrown away — so the
 * publisher's counters must stay still while the receiver's counter moves.
 *
 * The receiver here reads nothing until every message has been delivered, and
 * then finds the NEWEST 64 of the 70: for control the freshest command is the
 * authoritative one, so overflow drops the oldest. */
use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use Async\ThreadPool;
use function Async\spawn;
use function Async\await;
use function Async\delay;

$server = new HttpServer(
    (new HttpServerConfig())->addListener('127.0.0.1', 18076)
);
$server->enableRooms();

$room = $server->room('run/76/control');
$ack  = $server->room('test/ack76');

spawn(function () use ($server, $room, $ack) {
    $ack->subscribe();

    $pool = new ThreadPool(1);

    $task = $pool->submit(function () use ($room, $ack) {
        $room->subscribe();
        $ack->publish('subscribed');

        /* Idle, not receiving: the reactor drains the mailbox into the ring. */
        delay(700);

        $first = $room->recv(1000);
        $seen  = $first === null ? 0 : 1;

        while ($room->recv(0) !== null) {
            $seen++;
        }

        $lost = $room->lostCount();
        $room->unsubscribe();

        return "first=$first seen=$seen lost=$lost";
    });

    echo 'ack: ', $ack->recv(3000), "\n";

    $before = $server->getRuntimeStats();

    for ($i = 0; $i < 70; $i++) {
        $room->publish("m$i");
    }

    echo 'task: ', await($task), "\n";

    $after = $server->getRuntimeStats();

    echo 'sub_overflow moved by: ', $after['ws_sub_overflow'] - $before['ws_sub_overflow'], "\n";
    echo 'dropped moved by: ', $after['ws_topic_dropped'] - $before['ws_topic_dropped'], "\n";

    $pool->close();
    $ack->unsubscribe();
});
?>
--EXPECT--
ack: subscribed
task: first=m6 seen=64 lost=6
sub_overflow moved by: 6
dropped moved by: 0
