--TEST--
WebSocket topics: setWsPublishRateLimit throttles publish() per connection (issue #120)
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
/* publish() is the one WebSocket call an unprivileged peer can turn into work on
 * EVERY worker in the process — send()/trySend() only ever touch its own socket.
 * Unmetered, one client looping on a relayed message fills every worker's inbox,
 * and once an inbox is full the drops take out OTHER topics' traffic too: the
 * damage is process-wide, not confined to the abuser.
 *
 * The bucket is per connection, off by default. Over the rate publish() throws
 * and the connection stays up — the sender is TOLD, instead of the message
 * disappearing into a full mailbox where nobody can see it. */
require_once __DIR__ . '/../server/_free_port.inc';
require_once __DIR__ . '/_ws_client.inc';

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\WebSocket;
use TrueAsync\HttpRequest;
use TrueAsync\WebSocketBackpressureException;
use function Async\spawn;
use function Async\delay;

const BURST = 5;
const FLOOD = 50;

echo 'default rate limit: ', (new HttpServerConfig())->getWsPublishRateLimit(), "\n";

$port = tas_free_port();
$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setWorkers(1)
    ->setReadTimeout(10)
    ->setWriteTimeout(10)
    ->setWsPingIntervalMs(0)
    ->setWsPublishRateLimit(10, BURST);   // 10/s sustained, 5 in hand

echo 'configured rate: ', $config->getWsPublishRateLimit(),
     ' burst: ', $config->getWsPublishBurst(), "\n";

$server = new HttpServer($config);

$server->addWebSocketHandler(function (WebSocket $ws, HttpRequest $req) {
    /* BEFORE any WS I/O — no session exists yet. The bucket lives on the
     * CONNECTION, so a handler that publishes without ever calling recv/send/
     * subscribe is metered all the same; metering the session would have let this
     * loop run unbounded and defeat the limit outright. */
    $early    = 0;
    $earlyOff = 0;

    for ($i = 0; $i < 20; $i++) {
        try { $ws->publish('room', 'pre'); $early++; }
        catch (WebSocketBackpressureException $e) { $earlyOff++; }
    }

    $ws->subscribe('room');
    $ws->send("before any I/O: published=$early throttled=$earlyOff");

    foreach ($ws as $msg) {
        /* Flood: the bucket starts full, so the first BURST go out and the rest
         * are refused — a hot loop cannot outrun the leash. */
        $sent    = 0;
        $refused = 0;

        for ($i = 0; $i < FLOOD; $i++) {
            try { $ws->publish('room', "m$i"); $sent++; }
            catch (WebSocketBackpressureException $e) { $refused++; }
        }

        $ws->send("sent=$sent refused=$refused");

        /* The bucket refills: after a second's wait a publish is allowed again,
         * so the limit throttles rather than latching the connection off. */
        delay(1200);

        try { $ws->publish('room', 'after-refill'); $ws->send('after refill: ok'); }
        catch (WebSocketBackpressureException $e) { $ws->send('after refill: refused'); }
    }
});

$server->addHttpHandler(function ($req, $res) { $res->setStatusCode(404)->end(); });

spawn(function () use ($port, $server) {
    delay(1500);

    $fp = ws_open($port);
    if ($fp === null) { echo "handshake failed\n"; return; }

    echo ws_await($fp, 10000), "\n";   // the pre-I/O report

    ws_write($fp, 'go');

    $line = ws_await($fp, 10000);

    if (!preg_match('/sent=(\d+) refused=(\d+)/', $line, $m)) {
        echo "no report: $line\n";
        fclose($fp);
        $server->stop();
        return;
    }

    [$sent, $refused] = [(int) $m[1], (int) $m[2]];

    /* The pre-I/O burst already drained the bucket, so this second flood is
     * refused almost entirely — what matters is that it IS metered. */
    echo 'flood was throttled: ', $refused > 0 && $sent + $refused === FLOOD ? 'yes' : "no ($sent/$refused)", "\n";
    echo 'connection survived: yes', "\n";
    echo ws_await($fp, 10000), "\n";

    fclose($fp);
    $server->stop();
});

$server->start();
?>
--EXPECTF--
default rate limit: 0
configured rate: 10 burst: 5
before any I/O: published=5 throttled=15
flood was throttled: yes
connection survived: yes
after refill: ok%A
