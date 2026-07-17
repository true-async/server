--TEST--
Rooms: HttpServer::publish() from a coroutine (no WebSocket) reaches every subscriber; subscriberCount() tallies them
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
/* The server-side room API: a coroutine that holds NO WebSocket publishes to a
 * topic and every subscriber receives it — the producer of an app event (a
 * background job) is not a socket. Single worker here; 057 does it across
 * workers. Because there is no sending connection, nobody is excluded. */
require_once __DIR__ . '/../server/_free_port.inc';
require_once __DIR__ . '/_ws_client.inc';

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\WebSocket;
use TrueAsync\HttpRequest;
use function Async\spawn;
use function Async\delay;

const WATCHERS = 4;

$port = tas_free_port();
$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(10)
    ->setWriteTimeout(10)
    ->setWsPingIntervalMs(0);

$server = new HttpServer($config);
$server->enableRooms();

$server->addWebSocketHandler(function (WebSocket $ws, HttpRequest $req) {
    $ws->subscribe('projects/demo');

    foreach ($ws as $msg) {
        /* read-only watchers — they never publish */
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

    /* subscriberCount from the coroutine — no connection involved */
    $n = $server->subscriberCount('projects/demo');
    echo 'subscriberCount: ', $n === WATCHERS ? 'yes' : "no ($n)", "\n";

    /* server publish reaches every watcher, no one excluded */
    $server->publish('projects/demo', 'deploy');
    delay(500);

    $got = 0;
    foreach ($conns as $fp) {
        if (ws_read_pending($fp) === 'deploy') { $got++; }
    }
    echo 'all watchers received: ', $got === WATCHERS ? 'yes' : "no ($got)", "\n";

    /* a publish topic must be concrete */
    try {
        $server->publish('projects/#', 'x');
        echo "wildcard rejected: no\n";
    } catch (\Throwable $e) {
        echo "wildcard rejected: yes\n";
    }

    foreach ($conns as $fp) { fclose($fp); }

    $server->stop();
});

$server->start();
?>
--EXPECTF--
subscriberCount: yes
all watchers received: yes
wildcard rejected: yes%A
