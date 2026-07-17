--TEST--
Rooms: HttpServer::room() returns a Room handle whose name()/subscriberCount()/publish() drive the topic from the server side
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
/* The Room handle is the ergonomic face of the server-side room API: obtained
 * from $server->room($topic), it publishes and counts without a connection.
 * It only accepts a concrete topic. */
require_once __DIR__ . '/../server/_free_port.inc';
require_once __DIR__ . '/_ws_client.inc';

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\WebSocket;
use TrueAsync\HttpRequest;
use TrueAsync\Room;
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
    $ws->subscribe('projects/demo');

    foreach ($ws as $msg) {
        /* read-only watchers */
    }
});

$server->addHttpHandler(function ($req, $res) { $res->setStatusCode(404)->end(); });

spawn(function () use ($port, $server) {
    delay(1500);

    $room = $server->room('projects/demo');
    echo 'is Room: ', $room instanceof Room ? 'yes' : 'no', "\n";
    echo 'name: ', $room->name(), "\n";

    $conns = [];
    for ($i = 0; $i < WATCHERS; $i++) {
        $fp = ws_open($port);
        if ($fp === null) { echo "handshake failed\n"; $server->stop(); return; }
        $conns[] = $fp;
    }

    delay(400);

    echo 'subscriberCount: ', $room->subscriberCount() === WATCHERS ? 'yes' : 'no', "\n";

    $room->publish('via-room');
    delay(500);

    $got = 0;
    foreach ($conns as $fp) {
        if (ws_read_pending($fp) === 'via-room') { $got++; }
    }
    echo 'all received: ', $got === WATCHERS ? 'yes' : "no ($got)", "\n";

    try {
        $server->room('bad/#');
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
is Room: yes
name: projects/demo
subscriberCount: yes
all received: yes
wildcard rejected: yes%A
