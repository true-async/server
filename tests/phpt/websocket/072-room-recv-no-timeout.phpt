--TEST--
Rooms: recv() without a timeout is work, not a deadlock — a parked receiver survives an idle thread
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
/* The wake source is the thread's mailbox trigger, which does not hold the loop
 * open by itself: without the park keepalive both receivers below are cancelled
 * with "Deadlock detected" within a millisecond of parking. The two of them park
 * on one thread on purpose — the first one returning must not drop the ref the
 * second still needs. The one-second gaps are three orders of magnitude past the
 * cancellation they are here to disprove. */
use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use Async\ThreadPool;
use function Async\spawn;
use function Async\await;
use function Async\delay;

$server = new HttpServer(
    (new HttpServerConfig())->addListener('127.0.0.1', 18072)
);
$server->enableRooms();

$first  = $server->room('run/72/a');
$second = $server->room('run/72/b');
$ack    = $server->room('test/ack72');

spawn(function () use ($server, $first, $second, $ack) {
    $ack->subscribe();

    $pool = new ThreadPool(1);

    $task = $pool->submit(function () use ($first, $second, $ack) {
        $first->subscribe();
        $second->subscribe();

        $a = spawn(fn() => $first->recv());
        $b = spawn(fn() => $second->recv());

        $ack->publish('subscribed');

        $out = 'A:' . (await($a) ?? '(null)');
        $out .= ' B:' . (await($b) ?? '(null)');

        $first->unsubscribe();
        $second->unsubscribe();

        return $out;
    });

    echo 'ack: ', $ack->recv(3000), "\n";

    delay(1000);
    $first->publish('one');

    delay(1000);
    $second->publish('two');

    echo 'task: ', await($task), "\n";

    $stats = $server->getRuntimeStats();
    echo 'dropped: ', $stats['ws_topic_dropped'], "\n";

    $pool->close();
    $ack->unsubscribe();
});
?>
--EXPECT--
ack: subscribed
task: A:one B:two
dropped: 0
