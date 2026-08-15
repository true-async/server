--TEST--
Rooms reliable-send: single worker — publish() breakdown + trySend()/send() deliver locally without parking
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
/* One worker: the calling coroutine and the subscribers share it, so delivery is
 * entirely LOCAL — there is no remote mailbox to fill, so nothing parks. publish()
 * reports the local subscribers under 'served'; trySend() returns true; send()
 * counts those same local subscribers in what it returns (they are targets it
 * reached, not a road it took) and does NOT throw; all three reach the
 * subscribers. The
 * full-mailbox park path needs a wedged REMOTE worker and is out of reach in one
 * worker — see 063 for the real cross-worker delivery test, and the report for
 * what remains beyond a .phpt. */
require_once __DIR__ . '/../server/_free_port.inc';
require_once __DIR__ . '/_ws_client.inc';

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\WebSocket;
use TrueAsync\HttpRequest;
use function Async\spawn;
use function Async\delay;

const WATCHERS = 3;

$port = tas_free_port();
$server = new HttpServer(
    (new HttpServerConfig())
        ->addListener('127.0.0.1', $port)
        ->setReadTimeout(10)
        ->setWriteTimeout(10)
        ->setWsPingIntervalMs(0)
);
$server->enableRooms();

$server->addWebSocketHandler(function (WebSocket $ws, HttpRequest $req) {
    $ws->subscribe('ops/room');
    foreach ($ws as $msg) {
        /* read-only watchers */
    }
});

$server->addHttpHandler(function ($req, $res) { $res->setStatusCode(404)->end(); });

spawn(function () use ($port, $server) {
    delay(1500);

    $conns = [];
    for ($i = 0; $i < WATCHERS; $i++) {
        $fp = ws_open($port);
        if ($fp === null) { echo "handshake failed\n"; $server->stop(); return; }
        $conns[] = $fp;
    }

    delay(400);

    $reached = function (string $expect) use ($conns) {
        $got = 0;
        foreach ($conns as $fp) {
            if (ws_read_pending($fp) === $expect) { $got++; }
        }
        return $got;
    };

    $p = $server->publish('ops/room', 'p');
    echo 'publish shape: ', isset($p['served'], $p['posted'], $p['dropped']) ? 'ok' : 'bad', "\n";
    echo 'publish served: ', $p['served'] === WATCHERS ? 'yes' : "no ({$p['served']})", "\n";
    delay(300);
    echo 'publish reached: ', $reached('p') === WATCHERS ? 'yes' : 'no', "\n";

    $t = $server->trySend('ops/room', 't');
    echo 'trySend returned: ', var_export($t, true), "\n";
    delay(300);
    echo 'trySend reached: ', $reached('t') === WATCHERS ? 'yes' : 'no', "\n";

    $threw = false;
    $r = null;
    try { $r = $server->send('ops/room', 's'); }
    catch (\Throwable $e) { $threw = true; }
    echo 'send threw: ', $threw ? 'yes' : 'no', "\n";
    echo 'send delivered: ', $r === WATCHERS ? 'yes' : "no (" . var_export($r, true) . ")", "\n";
    delay(300);
    echo 'send reached: ', $reached('s') === WATCHERS ? 'yes' : 'no', "\n";

    $st = $server->getRuntimeStats();
    echo 'has retry stats: ',
        isset($st['ws_retry_queued'], $st['ws_retry_delivered'], $st['ws_retry_expired'],
              $st['ws_retry_rejected'], $st['ws_retry_gone'], $st['ws_retry_shutdown']) ? 'yes' : 'no', "\n";

    foreach ($conns as $fp) { fclose($fp); }
    $server->stop();
});

$server->start();
?>
--EXPECTF--
publish shape: ok
publish served: yes
publish reached: yes
trySend returned: true
trySend reached: yes
send threw: no
send delivered: yes
send reached: yes
has retry stats: yes%A
