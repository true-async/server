--TEST--
WebSocket: a room outlives the server that minted it — clean stop with $room still held
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
/* The single-worker path owns its hub outright, and $room is an ordinary PHP
 * object the script still holds when start() returns. Freeing the hub at stop
 * left every WebSocketRoom pointing at it — the destructor then took a mutex in
 * freed memory. The hub is refcounted for exactly this reason, so the room stays
 * usable after the server is gone. */
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
$room   = $server->room('lobby');

$server->addWebSocketHandler(function (WebSocket $ws, HttpRequest $req) use ($room) {
    $room->join($ws);

    foreach ($ws as $msg) {
        if ($msg->data === 'count') {
            $ws->send('count:' . $room->count());
            continue;
        }

        $room->broadcast('relay:' . $msg->data, $ws);
    }
});

$server->addHttpHandler(function ($req, $res) { $res->setStatusCode(404)->end(); });

spawn(function () use ($port, $server) {
    $a = ws_open($port);
    $b = ws_open($port);

    if ($a === null || $b === null) {
        echo "handshake failed\n";
        $server->stop();
        return;
    }

    delay(300);

    ws_write($a, 'count');
    delay(300);
    echo 'count: ', ws_read_pending($a), "\n";

    ws_write($a, 'hi');
    delay(300);
    echo 'peer got: ',        ws_read_pending($b), "\n";
    echo 'sender excluded: ', ws_read_pending($a) === '' ? 'yes' : 'no', "\n";

    fclose($a);
    fclose($b);

    delay(300);
    $server->stop();
});

$server->start();

/* The server is down. The room object is still alive — using it and then
 * dropping it must not touch the hub through a dangling pointer. */
echo 'room after stop: ', $room->getName(), "\n";
echo 'count after stop: ', $room->count(), "\n";

unset($room);
echo "done\n";
?>
--EXPECTF--
count: count:2
peer got: relay:hi
sender excluded: yes
room after stop: lobby
count after stop: 0
done%A
