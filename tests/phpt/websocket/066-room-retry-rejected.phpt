--TEST--
Rooms reliable-send: enqueue past the outbound-queue cap is refused (retry_rejected; trySend -> false)
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
/* Cap the per-worker outbound queue at ONE entry. Force the cross-worker post to
 * keep failing so nothing drains. The first trySend() parks its target (queue now
 * at cap); the second finds the queue full and is refused — it returns false and
 * ws_retry_rejected advances, with nothing parked for it.
 *
 * The queue only comes into it when the target is on another worker, so the
 * subscribers are opened until both workers hold one. */
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
        ->setWsPublishRetryQueueMax(1)   // room for exactly one parked entry
);
$server->enableRooms();

$server->addWebSocketHandler(function (WebSocket $ws, HttpRequest $req) use ($server) {
    if ($req->getPath() === '/ctl') {
        foreach ($ws as $msg) {
            if ($msg->data !== 'go') { continue; }

            \TrueAsync\__test_force_topic_post_full(1 << 28);   // never drains

            /* Short deadline so the parked entry expires soon after, not at stop(). */
            $a = $server->trySend('rt', 'x', 300);   // parks -> queue at cap
            $b = $server->trySend('rt', 'y', 300);   // cap reached -> refused

            \TrueAsync\__test_force_topic_post_full(0);

            $ws->send('a=' . ($a ? '1' : '0') . ' b=' . ($b ? '1' : '0'));
        }
        return;
    }

    $ws->subscribe('rt');
    foreach ($ws as $msg) { /* receive only */ }
});

$server->addHttpHandler(function ($req, $res) { $res->setStatusCode(404)->end(); });

spawn(function () use ($port, $server) {
    delay(4000);

    /* A cross-worker post is what the outbound queue exists for, so the test
     * needs both workers to hold a subscriber before it forces that post to
     * fail. Accepts decide where a connection lands and the server promises
     * nothing about it, so the spread is proved rather than assumed. */
    $subs = ws_open_spread_subscribers($server, $port, 'rt', 2);
    if ($subs === null) { echo "no subscriber on a remote worker\n"; $server->stop(); return; }

    $ctl = ws_open($port, '/ctl');
    if ($ctl === null) { echo "ctl handshake failed\n"; $server->stop(); return; }

    delay(1200);

    $before = $server->getRuntimeStats();

    ws_write($ctl, 'go');
    $reply = ws_await($ctl, 10000);

    $after = $server->getRuntimeStats();

    echo "reply: $reply\n";
    echo 'retry_rejected advanced: ',
        ($after['ws_retry_rejected'] - $before['ws_retry_rejected']) >= 1 ? 'yes' : 'no', "\n";

    /* Let the one parked entry expire before teardown so shutdown stays quiet. */
    delay(600);

    foreach ($subs as $fp) { @fclose($fp); }
    @fclose($ctl);
    $server->stop();
});

$server->start();
?>
--EXPECTF--
reply: a=1 b=0
retry_rejected advanced: yes%A
