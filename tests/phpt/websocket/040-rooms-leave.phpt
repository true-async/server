--TEST--
WebSocket: leave() removes a member, and a closing connection leaves every room by itself
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
/* Membership is thread-local state that a disconnect has to unwind: the session
 * is freed by the connection layer, so ws_session_destroy is what removes it
 * from every room it joined. An explicit leave() must be idempotent, and a
 * dropped socket must not leave a member behind in the count. */
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
$room   = $server->room('r');

$server->addWebSocketHandler(function (WebSocket $ws, HttpRequest $req) use ($room) {
    $room->join($ws);

    foreach ($ws as $msg) {
        switch ($msg->data) {
            case 'leave':
                $room->leave($ws);
                $room->leave($ws);   // idempotent — the second one is a no-op
                $ws->send('ok');
                break;

            case 'count':
                $ws->send('count:' . $room->count());
                break;
        }
    }
});

$server->addHttpHandler(function ($req, $res) { $res->setStatusCode(404)->end(); });

function room_count($fp): string {
    ws_write($fp, 'count');
    delay(300);

    return ws_read_pending($fp);
}

spawn(function () use ($port, $server) {
    $a = ws_open($port);
    $b = ws_open($port);
    $c = ws_open($port);

    if ($a === null || $b === null || $c === null) {
        echo "handshake failed\n";
        $server->stop();
        return;
    }

    delay(300);
    echo 'joined: ', room_count($c), "\n";

    /* A leaves but stays connected. */
    ws_write($a, 'leave');
    delay(300);
    echo 'after leave: ', ws_read_pending($a) === 'ok' ? room_count($c) : 'no ack', "\n";

    /* A broadcast must no longer reach A. */
    ws_write($c, 'x');
    delay(300);
    echo 'left member skipped: ', ws_read_pending($a) === '' ? 'yes' : 'no', "\n";

    /* B just drops — nothing calls leave() for it. */
    fclose($b);
    delay(500);
    echo 'after disconnect: ', room_count($c), "\n";

    fclose($a);
    fclose($c);

    delay(300);
    $server->stop();
});

$server->start();
echo "done\n";
?>
--EXPECTF--
joined: count:3
after leave: count:2
left member skipped: yes
after disconnect: count:1
done%A
