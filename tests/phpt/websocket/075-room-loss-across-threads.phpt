--TEST--
Rooms: a ring that overflows in another thread says so, keeps the newest 64, and costs one body per publish
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
 * The receiver reads nothing until every message has landed, and then finds the
 * NEWEST 64 of the 70: for control the freshest command is the authoritative
 * one, so overflow drops the oldest.
 *
 * "All 70 have landed" is a guarantee here, not a wait: the receiver parks on a
 * second room, and the publisher posts the go-ahead last. Everything rides one
 * mailbox from one producer, so FIFO puts the 70 in the ring before the wake-up
 * that releases the reader.
 *
 * The body count is the same publishes seen from the sending side: 71 publishes
 * to a node holding two receivers cost 71 bodies, not 142 — a body is shared by
 * every ring and every mailbox it reaches. */
use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use Async\ThreadPool;
use function Async\spawn;
use function Async\await;

$server = new HttpServer(
    (new HttpServerConfig())->addListener('127.0.0.1', 18076)
);
$server->enableRooms();

$room  = $server->room('run/76/control');
$echo  = $server->room('run/76/control');   /* a second receiver on the same node */
$go    = $server->room('run/76/go');
$ack   = $server->room('test/ack76');

spawn(function () use ($server, $room, $echo, $go, $ack) {
    $ack->subscribe();

    $pool = new ThreadPool(1);

    $task = $pool->submit(function () use ($room, $echo, $go, $ack) {
        $room->subscribe();
        $echo->subscribe();
        $go->subscribe();

        $ack->publish('subscribed');

        $go->recv(5000);   /* released only after the 70 are in the ring */

        $first = $room->recv(1000);
        $seen  = $first === null ? 0 : 1;

        while ($room->recv(0) !== null) {
            $seen++;
        }

        $lost = $room->lostCount();
        $room->unsubscribe();
        $echo->unsubscribe();
        $go->unsubscribe();

        return "first=$first seen=$seen lost=$lost";
    });

    echo 'ack: ', $ack->recv(3000), "\n";

    $before = $server->getRuntimeStats();

    for ($i = 0; $i < 70; $i++) {
        $room->publish("m$i");
    }

    $go->publish('go');

    echo 'task: ', await($task), "\n";

    $after = $server->getRuntimeStats();

    echo 'sub_overflow moved by: ', $after['ws_sub_overflow'] - $before['ws_sub_overflow'], "\n";
    echo 'dropped moved by: ', $after['ws_topic_dropped'] - $before['ws_topic_dropped'], "\n";
    echo 'bodies moved by: ', $after['ws_bodies'] - $before['ws_bodies'], "\n";

    $pool->close();
    $ack->unsubscribe();
});
?>
--EXPECT--
ack: subscribed
task: first=m6 seen=64 lost=6
sub_overflow moved by: 12
dropped moved by: 0
bodies moved by: 71
