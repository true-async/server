--TEST--
WebSocket topics: a stalled peer on a remote worker drops its own traffic and stalls nobody else
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
/* 049 pins the drop on ONE worker, where the publisher's own thread does the
 * sending. The pool path is a different one: the payload goes through the
 * mailbox, and the delivery — the ws_session_try_send that has to drop rather
 * than queue — happens on the RECEIVING worker's reactor, inside the drain.
 *
 * Two properties, and the second is the one that matters:
 *   1. publish() still never suspends. The publisher's loop runs to completion
 *      even while peers on other workers are backed up — a park there would hang
 *      the publishing worker on a peer it does not even own.
 *   2. A peer that stopped reading takes only ITSELF down. Once the healthy peers
 *      are drained, the next publish reaches every one of them — the stalled
 *      sockets did not wedge the workers they live on.
 *
 * Four workers, eight peers: some never read a byte, the rest do. */
require_once __DIR__ . '/../server/_free_port.inc';
require_once __DIR__ . '/_ws_client.inc';

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\WebSocket;
use TrueAsync\HttpRequest;
use function Async\spawn;
use function Async\delay;

const STALLED = 4;   /* peers that never read */
const HEALTHY = 3;   /* peers that do */

$port = tas_free_port();
$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setWorkers(4)
    ->setReadTimeout(15)
    ->setWriteTimeout(15)
    ->setWsPingIntervalMs(0)
    ->setStreamWriteBufferBytes(4096);   /* small high-water mark → backpressure fast */

$server = new HttpServer($config);

/* Self-contained: a worker thread gets a fresh PHP context and does not inherit
 * this script's constants. */
$server->addWebSocketHandler(function (WebSocket $ws, HttpRequest $req) {
    $ws->subscribe('flood');

    foreach ($ws as $msg) {
        if ($msg->data === 'flood') {
            $payload = str_repeat('x', 2048);

            for ($i = 0; $i < 300; $i++) {
                $ws->publish('flood', $payload);
            }

            /* Reaching this at all is property 1: a publish that parked on a
             * backed-up peer — on ANY worker — would never come back. */
            $ws->send('flood done');
            continue;
        }

        if ($msg->data === 'marker') {
            $ws->publish('flood', 'MARKER');
            $ws->send('marker sent');
        }
    }
});

$server->addHttpHandler(function ($req, $res) { $res->setStatusCode(404)->end(); });

spawn(function () use ($port, $server) {
    delay(4000);   // four workers have to bind

    /* conns[0] publishes; it is subscribed too, but excludeSelf keeps it out. */
    $conns = [];
    for ($i = 0; $i < 1 + STALLED + HEALTHY; $i++) {
        $fp = ws_open($port);

        if ($fp === null) { echo "handshake failed\n"; $server->stop(); return; }

        $conns[] = $fp;
    }

    delay(1000);   // let every subscribe land in its worker's tree

    $stalled = array_slice($conns, 1, STALLED);            // never read
    $healthy = array_slice($conns, 1 + STALLED, HEALTHY);  // drained below

    ws_write($conns[0], 'flood');

    $done = ws_await($conns[0], 20000);

    echo 'publisher completed the burst: ', $done === 'flood done' ? 'yes' : "no ($done)", "\n";

    /* Drain the healthy peers of whatever the flood managed to deliver, so the
     * marker below cannot be confused with a leftover. The stalled ones stay
     * untouched — still backed up, still subscribed. */
    foreach ($healthy as $fp) {
        while (ws_read_pending($fp) !== '') {
            // discard
        }
    }

    ws_write($conns[0], 'marker');
    ws_await($conns[0], 10000);

    $reached = 0;
    foreach ($healthy as $fp) {
        $frame = ws_await($fp, 5000);
        $reached += $frame === 'MARKER' ? 1 : 0;
    }

    echo 'healthy peers still served past the stalled ones: ', $reached, '/', HEALTHY, "\n";
    echo 'stalled peers still connected: ', count(array_filter($stalled, fn($fp) => !feof($fp))),
         '/', STALLED, "\n";

    foreach ($conns as $fp) { fclose($fp); }

    $server->stop();
});

$server->start();
?>
--EXPECTF--
publisher completed the burst: yes
healthy peers still served past the stalled ones: 3/3
stalled peers still connected: 4/4%A
