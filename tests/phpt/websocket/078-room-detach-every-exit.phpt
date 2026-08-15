--TEST--
Rooms: however a task ends — normally, throwing, cancelled — its worker keeps a whole attachment and leaves no slot behind
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
use Async\ThreadPool;
use function Async\spawn;
use function Async\await;
use function Async\delay;

/* The attachment belongs to the THREAD, not to the task: a pool worker serves one
 * request for its whole life, so a task that dies must leave the worker's slot,
 * mailbox and tree exactly as they were — and the thread's own exit must then give
 * the slot back. Two failures are being watched for, and they are opposites:
 *   - a task's death half-dismantles the attachment, so the next task on that
 *     worker is subscribed to nothing and receives nothing;
 *   - the thread ends without detaching, so the slot stays taken for the life of
 *     the process, collecting messages nobody will read (S1.9's contagion).
 * `workers` from publish() counts live slots, which is what makes both visible. */
$server = new HttpServer(
    (new HttpServerConfig())->addListener('127.0.0.1', 18078)
);
$server->enableRooms();

$room = $server->room('run/3/control');
$ack  = $server->room('run/3/ack');

/* A worker's exit is not synchronous with the future that settled its task, so
 * the slot is polled rather than assumed. */
function workers_settle(TrueAsync\Room $room, int $want): string
{
    for ($waited = 0; $waited < 3000; $waited += 50) {
        if ($room->publish('poll')['workers'] === $want) {
            return 'yes';
        }

        delay(50);
    }

    return 'NO (' . $room->publish('poll')['workers'] . ')';
}

spawn(function () use ($server, $room, $ack) {
    $room->subscribe();
    $ack->subscribe();

    echo 'workers, this thread alone: ', $room->publish('x')['workers'], "\n";

    $pool = new ThreadPool(1, coroutine: true);

    /* 1. A task that ends normally. */
    $first = $pool->submit(function () use ($room, $ack) {
        $room->subscribe();
        $ack->publish('one');

        return 'ok';
    });

    echo 'ack: ', $ack->recv(3000), "\n";
    echo 'workers with the task alive: ', $room->publish('x')['workers'], "\n";
    echo 'normal end: ', await($first), "\n";

    /* 2. A task that throws — on the same worker, whose attachment must survive. */
    $second = $pool->submit(function () use ($room, $ack) {
        $room->subscribe();
        $ack->publish('two');

        throw new \RuntimeException('boom');
    });

    echo 'ack: ', $ack->recv(3000), "\n";

    try {
        await($second);
        echo "throwing task: returned\n";
    } catch (\Throwable $e) {
        echo 'throwing task: ', $e::class, "\n";
    }

    echo 'workers after the throw: ', $room->publish('x')['workers'], "\n";

    /* 3. A task cancelled while parked in recv(), then the reuse check: the next
     * task on that worker must still be able to join the room and receive. */
    $third = $pool->submit(function () use ($room, $ack) {
        $room->subscribe();
        $ack->publish('three');

        return $room->recv(10000) ?? '(timeout)';
    });

    echo 'ack: ', $ack->recv(3000), "\n";
    $third->cancel();

    try {
        await($third);
        echo "cancelled task: returned\n";
    } catch (\Throwable $e) {
        echo 'cancelled task: ', $e::class, "\n";
    }

    $fourth = $pool->submit(function () use ($room, $ack) {
        $room->subscribe();
        $ack->publish('four');

        return 'got:' . ($room->recv(3000) ?? '(timeout)');
    });

    echo 'ack: ', $ack->recv(3000), "\n";
    delay(50);
    echo 'send after three deaths: ', $room->send('still here', 2000), "\n";
    echo 'reused worker: ', await($fourth), "\n";

    /* The thread's exit is the detach. */
    $pool->close();
    echo 'slot given back: ', workers_settle($room, 1), "\n";

    $stats = $server->getRuntimeStats();
    echo 'dropped: ', $stats['ws_topic_dropped'], "\n";
    echo 'retry_expired: ', $stats['ws_retry_expired'], "\n";

    $room->unsubscribe();
    $ack->unsubscribe();
});
?>
--EXPECT--
workers, this thread alone: 1
ack: one
workers with the task alive: 2
normal end: ok
ack: two
throwing task: RuntimeException
workers after the throw: 2
ack: three
cancelled task: Async\AsyncCancellation
ack: four
send after three deaths: 2
reused worker: got:still here
slot given back: yes
dropped: 0
retry_expired: 0
