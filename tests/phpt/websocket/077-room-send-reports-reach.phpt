--TEST--
Rooms: a reliable send counts the subscribers it served here, and says whether there was any worker to reach
--SKIPIF--
<?php
if (!PHP_ZTS) die('skip ZTS required');
?>
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\RoomDeliveryException;
use function Async\spawn;

/* Three answers a send used to be unable to give:
 *   - with no thread attached to the hub the message had nowhere to go, and a
 *     later attach cannot rescue it, so this is an error rather than a delivered-
 *     to-nobody OK;
 *   - a room whose whole membership sits on the CALLING thread was served, and
 *     `delivered` now says so instead of counting remote mailboxes alone;
 *   - an empty room on a live worker is a plain 0 — no error, because a
 *     subscriber may yet join. */
$server = new HttpServer(
    (new HttpServerConfig())->addListener('127.0.0.1', 18077)
);
$server->enableRooms();

$room = $server->room('run/9/control');

spawn(function () use ($server, $room) {
    try {
        $room->send('stop', 200);
        echo "no worker: NOT refused\n";
    } catch (RoomDeliveryException $e) {
        echo 'no worker: refused, delivered=', $e->delivered, ' pending=', $e->pending, "\n";
    }

    /* subscribe() attaches this thread, so from here on the hub has a worker. */
    $room->subscribe();

    echo 'local send delivered: ', $room->send('stop', 200), "\n";
    echo 'and the message is here: ', $room->recv(0) ?? '(lost)', "\n";

    $audit = $server->room('run/9/audit');

    echo 'empty room delivered: ', $audit->send('note', 200), "\n";

    $p = $audit->publish('note');
    echo 'empty room publish: served=', $p['served'], ' workers=', $p['workers'], "\n";

    $room->unsubscribe();
});
?>
--EXPECT--
no worker: refused, delivered=0 pending=0
local send delivered: 1
and the message is here: stop
empty room delivered: 0
empty room publish: served=0 workers=1
