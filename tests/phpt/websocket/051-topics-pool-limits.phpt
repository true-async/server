--TEST--
WebSocket topics: setWsMaxSubscriptions and setWsPublishRateLimit reach the worker clones (pool mode)
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
/* Both limits are read out of the server config on the thread that owns the
 * connection — and in pool mode that thread is a worker CLONE, whose config was
 * copied from the parent's. A field left out of one of the three copy paths in
 * http_server_config.c would leave the limit silently off for every connection in
 * the process, while a single-worker test (045, 050) stayed green: there the
 * handler runs on the parent's own config.
 *
 * So this one runs four workers and asserts the limits still bite. */
require_once __DIR__ . '/../server/_free_port.inc';
require_once __DIR__ . '/_ws_client.inc';

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\WebSocket;
use TrueAsync\HttpRequest;
use TrueAsync\WebSocketException;
use TrueAsync\WebSocketBackpressureException;
use function Async\spawn;
use function Async\delay;

const CAP   = 3;    /* distinct filters per connection */
const BURST = 5;    /* publishes in hand before the bucket runs dry */

$port = tas_free_port();
$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setWorkers(4)
    ->setReadTimeout(10)
    ->setWriteTimeout(10)
    ->setWsPingIntervalMs(0)
    ->setWsMaxSubscriptions(CAP)
    ->setWsPublishRateLimit(10, BURST);

$server = new HttpServer($config);

/* The handler runs on a worker thread with a FRESH PHP context — it does not
 * inherit this script's constants, so the numbers are inline here. CAP and BURST
 * below exist for the client side, which runs on the parent. */
$server->addWebSocketHandler(function (WebSocket $ws, HttpRequest $req) {
    $subscribed = 0;
    $refused    = 0;

    for ($i = 0; $i < 6; $i++) {              // cap is 3
        try { $ws->subscribe("t/$i"); $subscribed++; }
        catch (WebSocketException $e) { $refused++; }
    }

    $published = 0;
    $throttled = 0;

    for ($i = 0; $i < 30; $i++) {             // burst is 5
        try { $ws->publish('t/0', 'x'); $published++; }
        catch (WebSocketBackpressureException $e) { $throttled++; }
    }

    $ws->send("subscribed=$subscribed refused=$refused "
            . "published=$published throttled=$throttled "
            . "held=" . count($ws->getTopics()));

    foreach ($ws as $ignored) {
        // stay up so the client can read the report
    }
});

$server->addHttpHandler(function ($req, $res) { $res->setStatusCode(404)->end(); });

spawn(function () use ($port, $server) {
    delay(4000);   // four workers have to bind

    /* Two clients: whichever worker each lands on, both must be metered. */
    $reports = [];

    for ($c = 0; $c < 2; $c++) {
        $fp = ws_open($port);

        if ($fp === null) { echo "handshake failed\n"; $server->stop(); return; }

        $reports[] = ws_await($fp, 15000);

        fclose($fp);
    }

    foreach ($reports as $i => $line) {
        $ok = $line === 'subscribed=' . CAP . ' refused=3 '
                     . 'published=' . BURST . ' throttled=' . (BURST * 6 - BURST) . ' '
                     . 'held=' . CAP;

        echo 'client ', $i, ' metered on its worker: ', $ok ? 'yes' : "no ($line)", "\n";
    }

    $server->stop();
});

$server->start();
?>
--EXPECTF--
client 0 metered on its worker: yes
client 1 metered on its worker: yes%A
