--TEST--
Rooms: a ThreadPool task subscribes to a room and receives what another thread publishes
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
/* The receive side of a room: the subscriber is a coroutine's queue rather than
 * a WebSocket connection, so a run executing in a pool thread can be told
 * something. Two handles on one topic prove that unsubscribing one leaves the
 * other receiving — they share the thread's attachment. */
use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use Async\ThreadPool;
use function Async\spawn;
use function Async\await;

$server = new HttpServer(
    (new HttpServerConfig())->addListener('127.0.0.1', 18070)
);
$server->enableRooms();

$control = $server->room('run/42/control');
$second  = $server->room('run/42/control');
$ack     = $server->room('test/ack');

spawn(function () use ($server, $control, $second, $ack) {
    $ack->subscribe();

    $pool = new ThreadPool(1);

    $task = $pool->submit(function () use ($control, $second, $ack) {
        $control->subscribe();
        $second->subscribe();
        $ack->publish('subscribed');

        $out = [];
        $out[] = 'A:' . ($control->recv(3000) ?? '(timeout)');
        $out[] = 'B:' . ($second->recv(3000) ?? '(timeout)');

        $control->unsubscribe();
        $ack->publish('unsubscribed');

        $out[] = 'B2:' . ($second->recv(3000) ?? '(timeout)');

        try {
            $control->recv(100);
            $out[] = 'A2:not refused';
        } catch (\Throwable $e) {
            $out[] = 'A2:refused';
        }

        return implode(' ', $out);
    });

    echo 'ack: ', $ack->recv(3000), "\n";
    $control->publish('one');

    echo 'ack: ', $ack->recv(3000), "\n";
    $control->publish('two');

    echo 'task: ', await($task), "\n";

    $stats = $server->getRuntimeStats();
    echo 'dropped: ', $stats['ws_topic_dropped'], "\n";
    echo 'sub_overflow: ', $stats['ws_sub_overflow'], "\n";

    $pool->close();
    $ack->unsubscribe();
});
?>
--EXPECT--
ack: subscribed
ack: unsubscribed
task: A:one B:one B2:two A2:refused
dropped: 0
sub_overflow: 0
