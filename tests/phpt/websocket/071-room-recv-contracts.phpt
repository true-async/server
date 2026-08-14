--TEST--
Rooms: recv() contracts — zero timeout, a second parked recv, unsubscribe under a park, monotonic losses
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
use function Async\spawn;
use function Async\await;
use function Async\delay;

$server = new HttpServer(
    (new HttpServerConfig())->addListener('127.0.0.1', 18071)
);
$server->enableRooms();

$room = $server->room('run/7/control');

spawn(function () use ($server, $room) {
    $room->subscribe();

    /* 0 means "whatever is queued", never a wait. */
    $started = microtime(true);
    $empty   = $room->recv(0);
    $elapsed = (microtime(true) - $started) * 1000;
    echo 'zero timeout: ', $empty === null ? 'null' : $empty,
         ' in ', $elapsed < 100 ? 'no time' : 'A WAIT', "\n";

    /* A parked recv owns the subscription even once a message is queued in it. */
    $parked = spawn(fn() => $room->recv(3000));
    delay(50);
    $room->publish('first');

    try {
        $room->recv(1000);
        echo "second recv: NOT refused\n";
    } catch (\Throwable $e) {
        echo 'second recv: refused (', $e::class, ")\n";
    }

    echo 'parked got: ', await($parked), "\n";

    /* Unsubscribing under a parked recv must wake it, not free it under itself. */
    $waiting = spawn(function () use ($room) {
        try {
            $room->recv(5000);
            return 'returned';
        } catch (\Throwable $e) {
            return 'threw: ' . $e->getMessage();
        }
    });

    delay(50);
    $room->unsubscribe();

    echo 'under unsubscribe: ', await($waiting), "\n";

    /* A cancellation landing in the same turn as a delivery must not consume the
     * message: the cancelled coroutine cannot use it, the next reader can. */
    $room->subscribe();

    $doomed = spawn(fn() => $room->recv(5000));
    delay(50);
    $room->publish('survivor');
    $doomed->cancel();

    try {
        await($doomed);
        echo "cancelled recv: returned\n";
    } catch (\Throwable $e) {
        echo "cancelled recv: cancelled\n";
    }

    echo 'message survived: ', $room->recv(0) ?? '(lost)', "\n";
    $room->unsubscribe();

    /* Losses survive an unsubscribe: the counter belongs to the handle. */
    $room->subscribe();

    for ($i = 0; $i < 70; $i++) {
        $room->publish("m$i");
    }

    $lost = $room->lostCount();
    echo 'lost after 70 into a 64 ring: ', $lost, "\n";

    $room->unsubscribe();
    echo 'lost after unsubscribe: ', $room->lostCount(), "\n";

    $room->subscribe();
    echo 'lost after resubscribe: ', $room->lostCount(), "\n";
    $room->unsubscribe();
});
?>
--EXPECT--
zero timeout: null in no time
second recv: refused (TrueAsync\HttpServerRuntimeException)
parked got: first
under unsubscribe: threw: Room::recv() was interrupted: the subscription closed
cancelled recv: cancelled
message survived: survivor
lost after 70 into a 64 ring: 6
lost after unsubscribe: 6
lost after resubscribe: 6
