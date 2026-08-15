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

/* Neither a worker's exit nor a dead task's cleanup is synchronous with the
 * future that settled the task, so both figures are polled rather than assumed.
 * Polling cannot paper over a leak: a subscription nobody released never reaches
 * the expected count, and the wait ends in NO. */
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

function subs_settle(TrueAsync\Room $room, int $want): string
{
    for ($waited = 0; $waited < 3000; $waited += 50) {
        if ($room->subscriberCount(2000) === $want) {
            return 'yes';
        }

        delay(50);
    }

    return 'NO (' . $room->subscriberCount(2000) . ')';
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

    /* `workers` says the attachment is there; it cannot say what is left INSIDE
     * it. A task that dies must give back its own subscription — its room object
     * is freed with the task — and a count that stays at 1 (this thread's) is what
     * says so. A leak here is one dead subscription per task on a worker that
     * lives for the process. */
    echo 'subscriptions back to this thread alone: ', subs_settle($room, 1), "\n";

    /* 3. A task cancelled while parked in recv(), then the reuse check: the next
     * task on that worker must still be able to join the room and receive. */
    /* The ack goes out from a SECOND coroutine on that worker, which can only be
     * scheduled once the first one has parked — so the cancellation that follows
     * the ack lands on a parked recv rather than on a task still on its way there.
     * Under the valgrind lane the worker is tens of times slower than its
     * canceller, which is exactly when that ordering stops happening by luck. */
    $third = $pool->submit(function () use ($room, $ack) {
        $room->subscribe();
        spawn(function () use ($ack) {
            $ack->publish('three');
        });

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

        /* Generous: the checks between the ack and the send below poll two
         * cross-worker figures, and the point here is what arrives, not how fast. */
        return 'got:' . ($room->recv(15000) ?? '(timeout)');
    });

    echo 'ack: ', $ack->recv(3000), "\n";
    delay(50);
    echo 'send after three deaths: ', $room->send('still here', 2000), "\n";
    echo 'reused worker: ', await($fourth), "\n";

    /* The thread's exit is the detach: the slot goes, and with it every
     * subscription still held there.
     *
     * Measured, and deliberately not asserted before this point: a task cancelled
     * while parked keeps its room object — and so its subscription — for as long
     * as the pool holds that task's closure, which outlives the task itself when
     * another one is submitted straight after (5 s of polling, unchanged, until
     * the pool closed). That lifetime belongs to Async\ThreadPool, not to rooms;
     * what is ours is that the sweep takes it in the end. */
    $pool->close();
    echo 'slot given back: ', workers_settle($room, 1), "\n";
    echo 'subscriptions given back: ', subs_settle($room, 1), "\n";

    $room->unsubscribe();
    $ack->unsubscribe();

    $stats = $server->getRuntimeStats();
    echo 'dropped: ', $stats['ws_topic_dropped'], "\n";
    echo 'retry_expired: ', $stats['ws_retry_expired'], "\n";

    /* Nothing is queued anywhere now, so every body ever made must have been
     * released — the gauge for a payload stranded in a ring or a mailbox, which a
     * leak checker cannot see (it is still pointed at). */
    echo 'bodies balanced: ',
        $stats['ws_bodies'] === $stats['ws_bodies_freed']
            ? 'yes'
            : "no ({$stats['ws_bodies']} allocated, {$stats['ws_bodies_freed']} freed)", "\n";
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
subscriptions back to this thread alone: yes
ack: three
cancelled task: Async\AsyncCancellation
ack: four
send after three deaths: 2
reused worker: got:still here
slot given back: yes
subscriptions given back: yes
dropped: 0
retry_expired: 0
bodies balanced: yes
