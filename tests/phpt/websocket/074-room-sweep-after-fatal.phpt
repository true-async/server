--TEST--
Rooms: a worker killed by a fatal error leaves no slot taken and no mailbox nobody will read
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
/* The worker dies in the shape that costs the most: a receiver parked on a room
 * whose ring also holds messages nobody took. That receiver's reference on the
 * subscription dies with it, so nothing will ever bring the refcount to zero and
 * release the queued payloads — which are persistent memory, not the request's.
 * The teardown has to empty the ring itself, and the valgrind lane is what sees
 * it; from PHP only the slot is observable, which is the rest of this test.
 *
 * The fatal is E_USER_ERROR rather than an exhausted memory_limit because
 * USE_ZEND_ALLOC=0 replaces the allocator that enforces the limit: under the
 * valgrind lane the worker would simply finish, and the test would measure
 * nothing while still passing. */
use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use Async\ThreadPool;
use function Async\spawn;
use function Async\await;
use function Async\delay;

$server = new HttpServer(
    (new HttpServerConfig())->addListener('127.0.0.1', 18075)
);
$server->enableRooms();

$room = $server->room('run/75/control');
$ack  = $server->room('test/ack75');

spawn(function () use ($server, $room, $ack) {
    $ack->subscribe();

    $pool = new ThreadPool(1);

    $doomed = $pool->submit(function () use ($room, $ack) {
        $room->subscribe();

        spawn(fn() => $room->recv());
        delay(50);                 /* let the receiver park */

        $ack->publish('parked');   /* a cross-thread post needs no reactor turn */

        /* Asks every other worker and parks on the answers: a reply that comes
         * home after the fatal must be disposed of, not fired into the dead. */
        spawn(fn() => $room->subscriberCount(2000));

        /* No reactor turn from here on, so what the main thread publishes piles
         * up in this worker's mailbox and is still there when the teardown drains
         * it — with the receiver dead and holding a reference to the ring. */
        $until = microtime(true) + 0.6;

        while (microtime(true) < $until) {
        }

        error_reporting(E_ALL & ~E_DEPRECATED);
        trigger_error('the worker dies holding a room', E_USER_ERROR);

        return 'not reached';
    });

    /* Received first, and written as ONE string: the doomed worker writes its
     * fatal to the same stream from another thread, and an echo that suspends
     * between its arguments — or writes them one by one — lets that text land
     * inside this line. Which side of the line it lands on is not ours to order:
     * under the valgrind lane it arrived BEFORE it, which is what the leading %A
     * in the expectation is for. */
    $parked = $ack->recv(3000);
    echo "ack: $parked\n";

    for ($i = 0; $i < 200; $i++) {
        $room->publish("q$i");
    }

    try {
        await($doomed);
        echo "doomed: returned\n";
    } catch (\Throwable $e) {
        echo 'doomed: ', $e::class, "\n";
    }

    /* The slot goes when the dead worker's request shuts down, and the task's
     * future settling does not wait for that: poll rather than assume. */
    $posted = 1;

    for ($waited = 0; $posted !== 0 && $waited < 3000; $waited += 50) {
        $posted = $room->publish('nobody home')['posted'];

        if ($posted !== 0) {
            delay(50);
        }
    }

    echo 'slot released: ', $posted === 0 ? 'yes' : 'no', "\n";

    $before = $server->getRuntimeStats();

    /* The dead worker's slot is gone, so this send reaches nobody — refused, and
     * refused for the room-is-empty reason rather than charged to a full mailbox
     * that no longer exists. */
    try {
        $room->send('reliable, nobody home', timeoutMs: 300);
        echo "send: NOT refused\n";
    } catch (\TrueAsync\RoomDeliveryException $e) {
        echo 'send: refused, delivered=', $e->delivered, "\n";
    }

    $after  = $server->getRuntimeStats();
    echo 'dropped moved: ', var_export($after['ws_topic_dropped'] !== $before['ws_topic_dropped'], true), "\n";
    echo 'retry_expired moved: ', var_export($after['ws_retry_expired'] !== $before['ws_retry_expired'], true), "\n";

    $pool->close();
    $ack->unsubscribe();

    /* What the dead worker was holding: 200 bodies in its mailbox and its
     * receiver's ring, persistent memory whose owner died with it. valgrind
     * cannot see this class — a body still pointed at from a leaked structure is
     * not "definitely lost" — so the balance is the instrument. Nothing is queued
     * anywhere by now, so the two counters must meet. */
    $st = $server->getRuntimeStats();
    echo 'bodies balanced: ',
        $st['ws_bodies'] === $st['ws_bodies_freed']
            ? 'yes'
            : "no ({$st['ws_bodies']} allocated, {$st['ws_bodies_freed']} freed)", "\n";
});
?>
--EXPECTF--
%Aack: parked
%Adoomed: Async\ThreadTransferException
slot released: yes
send: refused, delivered=0
dropped moved: false
retry_expired moved: false
bodies balanced: yes
