--TEST--
Rooms: a Room captured by a ThreadPool task publishes from that thread
--SKIPIF--
<?php
if (!PHP_ZTS) die('skip ZTS required');
if (!class_exists('Async\ThreadPool')) die('skip ThreadPool not available');
?>
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
/* A room transfers into a pool thread: the task gets its own handle on the same
 * hub, and its publish reaches a WebSocket client owned by the server thread.
 * Without the transfer handler the captured room arrives uninitialized. */
require_once __DIR__ . '/../server/_free_port.inc';
require_once __DIR__ . '/_ws_client.inc';

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\WebSocket;
use TrueAsync\HttpRequest;
use Async\ThreadPool;
use function Async\spawn;
use function Async\await;
use function Async\delay;

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
        /* read-only watcher */
    }
});

$server->addHttpHandler(function ($req, $res) { $res->setStatusCode(404)->end(); });

spawn(function () use ($port, $server) {
    delay(1500);

    $room = $server->room('projects/demo');

    $fp = ws_open($port);
    if ($fp === null) { echo "handshake failed\n"; $server->stop(); return; }

    delay(400);

    $pool = new ThreadPool(1);
    $task = $pool->submit(function () use ($room) {
        return $room->name() . ':' . $room->publish('from-pool')['posted'];
    });

    echo 'task: ', await($task), "\n";

    delay(500);
    echo 'client got: ', ws_read_pending($fp) ?? '(nothing)', "\n";

    $pool->close();
    fclose($fp);
    $server->stop();
});

$server->start();
?>
--EXPECTF--
task: projects/demo:1
client got: from-pool%A
