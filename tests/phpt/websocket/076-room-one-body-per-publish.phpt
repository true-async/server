--TEST--
Rooms: a publish costs one message body, whether it goes local, remote, or both
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
/* A body is refcounted and shared by every ring and every mailbox it reaches, so
 * the cost of a publish is one allocation regardless of how many subscribers it
 * finds or how far it travels. The mixed case is the one that regressed while
 * this was being written: the local walk made a body, released it, and the
 * cross-worker fan-out then made an identical second one. */
use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use Async\ThreadPool;
use function Async\spawn;
use function Async\await;

$server = new HttpServer(
    (new HttpServerConfig())->addListener('127.0.0.1', 18077)
);
$server->enableRooms();

$room  = $server->room('run/77/control');
$echo  = $server->room('run/77/control');
$ack   = $server->room('test/ack77');

$bodies = static function () use ($server): int {
    return $server->getRuntimeStats()['ws_bodies'];
};

spawn(function () use ($server, $room, $echo, $ack, $bodies) {
    $ack->subscribe();

    /* Nobody subscribed anywhere: nothing to carry, nothing to allocate. */
    $before = $bodies();
    $room->publish('into the void');
    echo 'no subscriber: ', $bodies() - $before, "\n";

    /* Two receivers on this thread's own node. */
    $room->subscribe();
    $echo->subscribe();

    $before = $bodies();
    $room->publish('local only');
    echo 'two local receivers: ', $bodies() - $before, "\n";
    echo 'both got it: ', $room->recv(0), ' ', $echo->recv(0), "\n";

    $pool = new ThreadPool(1);

    $task = $pool->submit(function () use ($room, $ack) {
        $room->subscribe();
        $ack->publish('subscribed');

        return $room->recv(3000) ?? '(timeout)';
    });

    echo 'ack: ', $ack->recv(3000), "\n";

    /* Local receivers AND a worker to post to. */
    $before = $bodies();
    $room->publish('both ways');
    echo 'local and remote: ', $bodies() - $before, "\n";

    echo 'remote got: ', await($task), "\n";
    echo 'local got: ', $room->recv(0), "\n";

    $pool->close();
    $room->unsubscribe();
    $echo->unsubscribe();
    $ack->unsubscribe();
});
?>
--EXPECT--
no subscriber: 0
two local receivers: 1
both got it: local only local only
ack: subscribed
local and remote: 1
remote got: both ways
local got: both ways
