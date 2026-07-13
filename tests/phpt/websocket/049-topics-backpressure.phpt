--TEST--
WebSocket topics: a publish to a stalled subscriber drops the message instead of queueing it, and never suspends
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
/* Delivery to a topic runs on the reactor, where there is no coroutine to park —
 * so it goes through the non-suspending sink (trySend semantics). A subscriber
 * whose socket is backed up therefore DROPS the message; it must not queue it,
 * or one dead reader would grow the worker without bound, and it must not
 * suspend, or a single slow peer would stall delivery to everyone else.
 *
 * publish() returns how many subscribers it actually served on this worker, so a
 * stalled peer shows up as a run of zeros — that is the observable. */
require_once __DIR__ . '/../server/_free_port.inc';
require_once __DIR__ . '/_ws_client.inc';

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\WebSocket;
use TrueAsync\HttpRequest;
use function Async\spawn;
use function Async\delay;

const BURST = 200;

$port = tas_free_port();
$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setWorkers(1)                     // one worker: the peer is local, no mailbox hop
    ->setReadTimeout(10)
    ->setWriteTimeout(10)
    ->setWsPingIntervalMs(0)            // no server PING traffic in the way
    ->setStreamWriteBufferBytes(4096);  // small high-water mark → backpressure fast

$server = new HttpServer($config);

$server->addWebSocketHandler(function (WebSocket $ws, HttpRequest $req) {
    $first = $ws->recv();

    if ($first === null) { return; }

    /* The stalled subscriber: subscribes and then never reads another byte. */
    if ($first->data === 'sub') {
        $ws->subscribe('flood');

        foreach ($ws as $ignored) {
            // parks here; the client sends nothing more
        }

        return;
    }

    /* The publisher. It is NOT subscribed, so every delivery counted here went to
     * the stalled peer. */
    $payload = str_repeat('x', 2048);

    $served  = 0;
    $dropped = 0;

    for ($i = 0; $i < BURST; $i++) {
        if ($ws->publish('flood', $payload) > 0) {
            $served++;
        } else {
            $dropped++;
        }
    }

    /* Reaching this line at all is the "never suspends" half: a publish that
     * parked on the stalled peer would never come back. */
    $ws->send("served=$served dropped=$dropped completed=yes");
});

$server->addHttpHandler(function ($req, $res) { $res->setStatusCode(404)->end(); });

spawn(function () use ($port, $server) {
    delay(1500);

    $stalled = ws_open($port);
    if ($stalled === null) { echo "handshake failed\n"; return; }

    ws_write($stalled, 'sub');
    delay(300);

    $pub = ws_open($port);
    if ($pub === null) { echo "handshake failed\n"; return; }

    ws_write($pub, 'go');

    $line = ws_await($pub, 15000);

    if (!preg_match('/served=(\d+) dropped=(\d+) completed=yes/', $line, $m)) {
        echo "publisher did not finish: $line\n";
        fclose($stalled);
        fclose($pub);
        $server->stop();
        return;
    }

    [$served, $dropped] = [(int) $m[1], (int) $m[2]];

    echo 'publisher completed the burst: yes', "\n";
    echo 'some reached the peer before it backed up: ', $served > 0 ? 'yes' : 'no', "\n";
    echo 'the rest were dropped, not queued: ', $dropped > 0 ? 'yes' : 'no', "\n";
    echo 'every message accounted for: ', $served + $dropped === BURST ? 'yes' : 'no', "\n";

    fclose($stalled);
    fclose($pub);

    $server->stop();
});

$server->start();
?>
--EXPECTF--
publisher completed the burst: yes
some reached the peer before it backed up: yes
the rest were dropped, not queued: yes
every message accounted for: yes%A
