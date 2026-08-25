--TEST--
Rooms reliable-send: a target that stays full past the deadline expires and send() throws (retry_expired)
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
/* Force the cross-worker post to keep failing (a huge N) with a short per-call
 * timeout: the parked target never lands, the drainer gives up at the deadline,
 * and a blocking send() throws RoomDeliveryException carrying the still-pending
 * count. ws_retry_expired advances. */
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

$server->addWebSocketHandler(function (WebSocket $ws, HttpRequest $req) use ($server) {
    if ($req->getPath() === '/ctl') {
        foreach ($ws as $msg) {
            if ($msg->data !== 'go') { continue; }

            /* Never let the retry land; short deadline so it expires promptly. */
            \TrueAsync\__test_force_topic_post_full(1 << 28);

            try {
                $server->send('rt', 'hi', 250);
                $ws->send('noexc');
            } catch (\TrueAsync\RoomDeliveryException $e) {
                $ws->send('exc=' . $e::class . ' pending=' . $e->pending);
            } catch (\Throwable $e) {
                $ws->send('other=' . $e::class);
            } finally {
                \TrueAsync\__test_force_topic_post_full(0);   // clear the fault
            }
        }
        return;
    }

    $ws->subscribe('rt');
    foreach ($ws as $msg) { /* receive only */ }
});

$server->addHttpHandler(function ($req, $res) { $res->setStatusCode(404)->end(); });

spawn(function () use ($port, $server) {
    delay(4000);

    /* A cross-worker post is what the drainer here gives up on, so the test
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
    echo 'retry_queued advanced: ',
        ($after['ws_retry_queued'] - $before['ws_retry_queued']) >= 1 ? 'yes' : 'no', "\n";
    echo 'retry_expired advanced: ',
        ($after['ws_retry_expired'] - $before['ws_retry_expired']) >= 1 ? 'yes' : 'no', "\n";
    echo 'retry_delivered stayed 0: ',
        ($after['ws_retry_delivered'] - $before['ws_retry_delivered']) === 0 ? 'yes' : 'no', "\n";

    foreach ($subs as $fp) { @fclose($fp); }
    @fclose($ctl);
    $server->stop();
});

$server->start();
?>
--EXPECTF--
reply: exc=TrueAsync\RoomDeliveryException pending=1
retry_queued advanced: yes
retry_expired advanced: yes
retry_delivered stayed 0: yes%A
