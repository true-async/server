--TEST--
WebSocket topics: unsubscribing across workers stops delivery AND gives the interest filter back
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
/* Two halves, and the second is the one nothing else pins.
 *
 * A subscriber lives on one worker, the publisher on another. When the
 * subscriber unsubscribes, the tree removal happens on ITS worker — the
 * publisher never learns of it directly. What the publisher does see is the
 * interest filter, and that is a COUNTING Bloom: subscribing increments its
 * buckets, unsubscribing decrements them.
 *
 * If the decrement ever leaked, nothing would break loudly — delivery would still
 * be correct, because the receiving worker matches its own tree. The filter would
 * just stay permanently "interested" and every publish would keep waking every
 * worker for nobody, forever. That is invisible to a delivery assertion, so it is
 * asserted directly here: once the last subscriber is gone, the workers go back to
 * being skipped. */
require_once __DIR__ . '/../server/_free_port.inc';
require_once __DIR__ . '/_ws_client.inc';

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\WebSocket;
use TrueAsync\HttpRequest;
use function Async\spawn;
use function Async\delay;

const PEERS = 7;

$port = tas_free_port();
$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setWorkers(4)
    ->setReadTimeout(10)
    ->setWriteTimeout(10)
    ->setWsPingIntervalMs(0);

$server = new HttpServer($config);

/* Self-contained: a worker thread gets a fresh PHP context. */
$server->addWebSocketHandler(function (WebSocket $ws, HttpRequest $req) {
    foreach ($ws as $msg) {
        [$verb, $arg] = explode(':', $msg->data, 2) + [1 => ''];

        switch ($verb) {
            case 'sub':
                $ws->subscribe($arg);
                $ws->send('ok');
                break;

            case 'unsub':
                $ws->unsubscribe($arg);
                $ws->send('ok held=' . count($ws->getTopics()));
                break;

            case 'pub':
                $ws->publish('news', $arg);
                break;
        }
    }
});

$server->addHttpHandler(function ($req, $res) { $res->setStatusCode(404)->end(); });

spawn(function () use ($port, $server) {
    delay(4000);   // four workers have to bind

    $conns = [];
    for ($i = 0; $i <= PEERS; $i++) {
        $fp = ws_open($port);

        if ($fp === null) { echo "handshake failed\n"; $server->stop(); return; }

        $conns[] = $fp;
    }

    /* conns[0] publishes; a topic of its own so excludeSelf has an id to exclude. */
    ws_write($conns[0], 'sub:ctl');
    ws_await($conns[0]);

    for ($i = 1; $i <= PEERS; $i++) {
        ws_write($conns[$i], 'sub:news');
        ws_await($conns[$i]);
    }

    delay(500);

    $drain = function (string $expect) use ($conns) {
        $got = 0;
        for ($i = 1; $i <= PEERS; $i++) {
            $hit = false;
            while (($frame = ws_read_pending($conns[$i])) !== '') {
                $hit = $hit || $frame === $expect;
            }
            $got += $hit ? 1 : 0;
        }

        return $got;
    };

    ws_write($conns[0], 'pub:first');
    delay(1000);
    echo 'while subscribed, peers reached: ', $drain('first'), '/', PEERS, "\n";

    /* Everybody leaves — on whatever worker each of them sits. */
    for ($i = 1; $i <= PEERS; $i++) {
        ws_write($conns[$i], 'unsub:news');
        ws_await($conns[$i]);
    }

    delay(500);

    $before = $server->getRuntimeStats();

    ws_write($conns[0], 'pub:second');
    delay(1000);

    $after = $server->getRuntimeStats();

    $posted  = $after['ws_topic_posted']  - $before['ws_topic_posted'];
    $skipped = $after['ws_topic_skipped'] - $before['ws_topic_skipped'];

    echo 'after unsubscribe, peers reached: ', $drain('second'), '/', PEERS, "\n";
    echo 'after unsubscribe, workers woken: ', $posted, "\n";
    echo 'after unsubscribe, workers skipped: ', $skipped > 0 ? 'yes' : 'no', "\n";

    foreach ($conns as $fp) { fclose($fp); }

    $server->stop();
});

$server->start();
?>
--EXPECTF--
while subscribed, peers reached: 7/7
after unsubscribe, peers reached: 0/7
after unsubscribe, workers woken: 0
after unsubscribe, workers skipped: yes%A
