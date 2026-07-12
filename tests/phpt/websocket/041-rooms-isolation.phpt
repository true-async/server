--TEST--
WebSocket: rooms are isolated from each other; broadcastBinary, getName, no public constructor
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
/* A single-room test passes even when routing is completely broken, so this one
 * runs two rooms at once: a broadcast into one must not surface in the other.
 * Also covers the binary fan-out (its own opcode + deflate branch) and the fact
 * that a room is only obtainable through HttpServer::room(). */
require_once __DIR__ . '/../server/_free_port.inc';
require_once __DIR__ . '/_ws_client.inc';

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\WebSocket;
use TrueAsync\WebSocketRoom;
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
$red    = $server->room('red');
$blue   = $server->room('blue');

echo 'names: ', $red->getName(), ',', $blue->getName(), "\n";

/* The same name must yield the same room, not a second one. */
echo 'same name, same room: ',
    $server->room('red')->getName() === $red->getName() ? 'yes' : 'no', "\n";

try {
    new WebSocketRoom();
    echo "constructed directly: yes\n";
} catch (Error $e) {
    echo "constructed directly: no\n";
}

$server->addWebSocketHandler(function (WebSocket $ws, HttpRequest $req) use ($red, $blue) {
    $room = $req->getPath() === '/blue' ? $blue : $red;
    $room->join($ws);

    foreach ($ws as $msg) {
        if ($msg->data === 'bin') {
            $room->broadcastBinary("\x00\x01\x02", $ws);
            continue;
        }

        $room->broadcast('relay:' . $msg->data, $ws);
    }
});

$server->addHttpHandler(function ($req, $res) { $res->setStatusCode(404)->end(); });

spawn(function () use ($port, $server, $red, $blue) {
    $r1 = ws_open($port, '/red');
    $r2 = ws_open($port, '/red');
    $b1 = ws_open($port, '/blue');

    if ($r1 === null || $r2 === null || $b1 === null) {
        echo "handshake failed\n";
        $server->stop();
        return;
    }

    delay(300);
    echo 'red count: ', $red->count(), ', blue count: ', $blue->count(), "\n";

    ws_write($r1, 'hi');
    delay(300);
    echo 'peer in same room: ', ws_read_pending($r2), "\n";
    echo 'other room untouched: ', ws_read_pending($b1) === '' ? 'yes' : 'no', "\n";

    ws_write($r1, 'bin');
    delay(300);
    stream_set_blocking($r2, false);
    $frame = ws_read_frame($r2);
    stream_set_blocking($r2, true);

    echo 'binary opcode: ', $frame['opcode'] ?? 0, ', bytes: ',
        bin2hex($frame['data'] ?? ''), "\n";

    fclose($r1);
    fclose($r2);
    fclose($b1);

    delay(300);
    $server->stop();
});

$server->start();
echo "done\n";
?>
--EXPECTF--
names: red,blue
same name, same room: yes
constructed directly: no
red count: 2, blue count: 1
peer in same room: relay:hi
other room untouched: yes
binary opcode: 2, bytes: 000102
done%A
