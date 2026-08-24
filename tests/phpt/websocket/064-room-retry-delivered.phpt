--TEST--
Rooms reliable-send: a full target parks then a retry lands it (retry_queued -> retry_delivered)
--SKIPIF--
<?php
if (!function_exists('TrueAsync\\__test_force_topic_post_full')) {
    echo "skip requires --enable-tas-test-hooks (fault-injection hook absent)";
}
?>
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
/* The fault-injection hook forces the next cross-worker post to fail as if the
 * target mailbox were full, so a blocking send() must PARK the target on the
 * outbound queue and the reactor drainer must retry it — which then lands, since
 * the very next post succeeds. We assert both counters move and every subscriber
 * gets the message.
 *
 * The scenario runs INSIDE a worker handler (only an attached worker owns an
 * outbound queue — the pool parent has none). The handler captures the server
 * object (a Room handle transferred to a worker is not valid there; the server
 * reaches the hub); getRuntimeStats() is read from the parent. */
require_once __DIR__ . '/../server/_free_port.inc';
require_once __DIR__ . '/_ws_client.inc';

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\WebSocket;
use TrueAsync\HttpRequest;
use function Async\spawn;
use function Async\delay;

$port = tas_free_port();
$server = new HttpServer(
    (new HttpServerConfig())
        ->addListener('127.0.0.1', $port)
        ->setWorkers(2)
        ->setReadTimeout(30)
        ->setWriteTimeout(30)
        ->setWsPingIntervalMs(0)
);
$server->enableRooms();

/* The handler captures the server (as 057 does) — a transferred Room handle is
 * not valid on the worker, but the server is; send() reaches the hub through it. */
/* The bound travels bound to the closure: a worker runs with its own set of
 * constants, and the parent's are not among them. The count is decided by how
 * many connections it took to reach both workers, so the handler is given a
 * ceiling it cannot exceed rather than the exact number. */
$subs_total = 64;

$server->addWebSocketHandler(function (WebSocket $ws, HttpRequest $req) use ($server, $subs_total) {
    if ($req->getPath() === '/ctl') {
        foreach ($ws as $msg) {
            if ($msg->data !== 'go') { continue; }

            /* Fail exactly the one cross-worker post this send makes (2 workers =>
             * one remote target); the retry that follows succeeds. */
            \TrueAsync\__test_force_topic_post_full(1);

            try {
                $r = $server->send('rt', 'hi', 2000);
                /* delivered counts every subscriber this worker served plus one
                 * post per remote worker, and the connections land on the two
                 * workers in whatever split the accepts gave — so the count is
                 * bounded rather than exact. At least the retried post landed;
                 * at most every subscriber is local bar the one behind it. */
                $ws->send('ret in range: ' . ($r >= 1 && $r <= $subs_total ? 'yes' : "no ($r)"));
            } catch (\Throwable $e) {
                $ws->send('exc=' . $e::class);
            }
        }
        return;
    }

    $ws->subscribe('rt');
    foreach ($ws as $msg) { /* receive only */ }
});

$server->addHttpHandler(function ($req, $res) { $res->setStatusCode(404)->end(); });

spawn(function () use ($port, $server) {
    delay(4000);   // both workers bind

    /* The post this test fails is the cross-worker one, so both workers have to
     * hold a subscriber first. Accepts decide where a connection lands and the
     * server promises nothing about it, so the spread is proved, not assumed. */
    $subs = ws_open_spread_subscribers($server, $port, 'rt', 2);
    if ($subs === null) { echo "no subscriber on a remote worker\n"; $server->stop(); return; }

    $ctl = ws_open($port, '/ctl');
    if ($ctl === null) { echo "ctl handshake failed\n"; $server->stop(); return; }

    $before = $server->getRuntimeStats();

    ws_write($ctl, 'go');
    $reply = ws_await($ctl, 10000);

    $after = $server->getRuntimeStats();

    echo "reply: $reply\n";
    echo 'retry_queued advanced: ',
        ($after['ws_retry_queued'] - $before['ws_retry_queued']) >= 1 ? 'yes' : 'no', "\n";
    echo 'retry_delivered advanced: ',
        ($after['ws_retry_delivered'] - $before['ws_retry_delivered']) >= 1 ? 'yes' : 'no', "\n";

    $got = 0;
    foreach ($subs as $fp) {
        if (ws_await($fp, 3000) === 'hi') { $got++; }
    }
    echo 'all subscribers received: ', $got === count($subs) ? 'yes' : "no ($got of " . count($subs) . ")", "\n";

    foreach ($subs as $fp) { @fclose($fp); }
    @fclose($ctl);
    $server->stop();
});

$server->start();
?>
--EXPECTF--
reply: ret in range: yes
retry_queued advanced: yes
retry_delivered advanced: yes
all subscribers received: yes%A
