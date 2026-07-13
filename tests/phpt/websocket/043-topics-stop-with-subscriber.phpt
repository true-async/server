--TEST--
WebSocket topics: stop() with a subscriber still connected tears down cleanly (no freed-tree walk)
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
/* A session unsubscribes itself when it is destroyed, and the destroying is done
 * by the scope drain in stop(). So the worker must let go of its topic tree
 * AFTER that drain, never before — detaching first frees the tree out from under
 * the very teardown that walks it, and every stop() with a live subscriber is a
 * SIGSEGV.
 *
 * The other topic tests fclose() their clients before stopping, so the sessions
 * are usually already gone and the bug hides. Here the clients stay connected and
 * subscribed right through stop(). */
require_once __DIR__ . '/../server/_free_port.inc';
require_once __DIR__ . '/_ws_client.inc';

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\WebSocket;
use TrueAsync\HttpRequest;
use function Async\spawn;
use function Async\delay;

$port = tas_free_port();
$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setWorkers(1)
    ->setReadTimeout(10)
    ->setWriteTimeout(10)
    ->setWsPingIntervalMs(0);

$server = new HttpServer($config);

$server->addWebSocketHandler(function (WebSocket $ws, HttpRequest $req) {
    $ws->subscribe('chat');
    $ws->subscribe('room/+/msg');   /* a second node, so the teardown walks more than one */

    foreach ($ws as $msg) {
        $ws->publish('chat', $msg->data);
    }
});

$server->addHttpHandler(function ($req, $res) { $res->setStatusCode(404)->end(); });

spawn(function () use ($port, $server) {
    delay(2000);

    $conns = [];
    for ($i = 0; $i < 3; $i++) {
        $fp = ws_open($port);
        if ($fp === null) { echo "handshake failed\n"; return; }
        $conns[] = $fp;
    }

    delay(500);
    echo "subscribers connected\n";

    /* Deliberately no fclose() — the sessions are alive and still in the tree. */
    $server->stop();
    echo "stop() returned\n";
});

$server->start();

echo "start() returned\n";
?>
--EXPECTF--
subscribers connected
stop() returned
start() returned%A
