--TEST--
WebSocket topics: the interest filter skips workers with no subscriber, and never skips a wildcard one
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
/* A publish used to wake EVERY worker, copy the payload into each mailbox and let
 * most of them find no subscriber and throw it away. Each worker now summarises
 * what it subscribes to (topic_hub.c) and a publisher skips the ones that certainly
 * do not match.
 *
 * The interesting half is what must NOT be skipped. A filter cannot be summarised
 * by its full name — `user/+/msg` is a predicate, not a topic — so the summary
 * holds the LEADING LITERAL PREFIX of each filter and the publisher probes every
 * level-prefix of its topic. Get that wrong and wildcard subscribers on other
 * workers go silent, which is exactly what this test would catch. */
require_once __DIR__ . '/../server/_free_port.inc';
require_once __DIR__ . '/_ws_client.inc';

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\WebSocket;
use TrueAsync\HttpRequest;
use function Async\spawn;
use function Async\delay;

const PEERS = 7;   /* over 4 workers, so a peer is certainly on another one */

$port = tas_free_port();
$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setWorkers(4)
    ->setReadTimeout(10)
    ->setWriteTimeout(10)
    ->setWsPingIntervalMs(0);

$server = new HttpServer($config);

$server->addWebSocketHandler(function (WebSocket $ws, HttpRequest $req) {
    foreach ($ws as $msg) {
        [$cmd, $arg] = explode(':', $msg->data, 2);

        if ($cmd === 'sub') {
            $ws->subscribe($arg);
            $ws->send('ok');
            continue;
        }

        [$topic, $payload] = explode('|', $arg, 2);
        $ws->publish($topic, $payload);
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

    /* conns[0] only publishes; it subscribes to a topic of its own so that it
     * has an id and excludeSelf has something to exclude. */
    ws_write($conns[0], 'sub:ctl');

    for ($i = 1; $i <= PEERS; $i++) {
        ws_write($conns[$i], 'sub:user/+/msg');
        ws_write($conns[$i], 'sub:alerts/#');
    }

    delay(1000);   // let every subscribe land in its worker's tree

    $publish = function (string $topic, string $payload) use ($conns) {
        ws_write($conns[0], "pub:$topic|$payload");
        delay(1000);
    };

    /* Drains every peer — ws_read_pending() returns one frame per call, and a
     * leftover subscribe ack would otherwise read as a delivery. */
    $received = function (string $expect) use ($conns) {
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

    $received('');   // discard the acks

    $stats = fn() => $server->getRuntimeStats();

    /* 1. Nobody subscribes to this. Every other worker must be skipped outright:
     *    not one mailbox post, not one wake-up. */
    $before = $stats();
    $publish('void/topic', 'x');
    $after = $stats();

    $posted  = $after['ws_topic_posted']  - $before['ws_topic_posted'];
    $skipped = $after['ws_topic_skipped'] - $before['ws_topic_skipped'];

    echo 'unsubscribed topic — workers woken: ', $posted, "\n";
    echo 'unsubscribed topic — workers skipped: ', $skipped > 0 ? 'yes' : 'no', "\n";
    echo 'unsubscribed topic — peers reached: ', $received('x'), "\n";

    /* 2. '+' is a predicate: the filter's literal prefix is `user`, and the topic
     *    carries it. Every peer must be reached, on whatever worker it sits.
     *
     *    Deliberately NOT asserted here: that the publish woke another worker.
     *    Whether it has to depends on where the kernel put the connections — with
     *    SO_REUSEPORT it spreads them, on the shared-fd path they can all land on
     *    one worker — so `ws_topic_posted` is not a property of the filter. What
     *    IS the filter's property is above (an unsubscribed topic wakes nobody)
     *    and below (a matching one reaches everybody). */
    $publish('user/42/msg', 'plus');

    echo "'+' subscribers reached: ", $received('plus'), '/', PEERS, "\n";

    /* 3. '#' swallows the whole remainder, so its prefix is `alerts` and the
     *    topic may be arbitrarily deeper than the filter. */
    $publish('alerts/deep/nested/path', 'hash');
    echo "'#' subscribers reached: ", $received('hash'), '/', PEERS, "\n";

    foreach ($conns as $fp) { fclose($fp); }

    $server->stop();
});

$server->start();
?>
--EXPECTF--
unsubscribed topic — workers woken: 0
unsubscribed topic — workers skipped: yes
unsubscribed topic — peers reached: 0
'+' subscribers reached: 7/7
'#' subscribers reached: 7/7%A
