--TEST--
Rooms: a publish in one thread reaches a recv() in another, with or without WebSocket built
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
/* The room core carries no WebSocket dependency, so this runs identically in a
 * build configured with --disable-websocket. Nothing here names a connection:
 * the server is never started, and the room is the whole mechanism.
 *
 * The ack room sequences the two threads. A publish reaches a mailbox in FIFO
 * order, so "the task has subscribed" arriving here means the subscription is
 * live before the next publish leaves. A sleep would be a hope. */
use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use Async\ThreadPool;
use function Async\spawn;
use function Async\await;

$server = new HttpServer(
    (new HttpServerConfig())->addListener('127.0.0.1', 18081)
);
$server->enableRooms();

$control = $server->room('run/7/control');
$ack     = $server->room('test/ack');

spawn(function () use ($server, $control, $ack) {
    $ack->subscribe();

    $pool = new ThreadPool(1);

    $task = $pool->submit(function () use ($control, $ack) {
        $control->subscribe();
        $ack->publish('subscribed');

        $got = $control->recv(3000) ?? '(timeout)';

        $control->unsubscribe();

        return $got;
    });

    echo 'ack: ', $ack->recv(3000), "\n";

    $result = $control->send('stop');
    echo 'send reached: ', $result, "\n";

    echo 'task: ', await($task), "\n";

    /* posted counts what crossed a worker mailbox, so it is the number that
     * separates "another thread received it" from "served on this one". */
    $stats = $server->getRuntimeStats();
    echo 'posted: ', $stats['ws_topic_posted'], "\n";
    echo 'dropped: ', $stats['ws_topic_dropped'], "\n";
    echo 'lost: ', $control->lostCount(), "\n";

    $pool->close();
    $ack->unsubscribe();
});
?>
--EXPECT--
ack: subscribed
send reached: 1
task: stop
posted: 2
dropped: 0
lost: 0
