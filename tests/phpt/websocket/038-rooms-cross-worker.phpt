--TEST--
WebSocket: rooms fan a broadcast across workers, and count() tallies every worker
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
/* A worker is a thread with its own PHP context, so a room cannot be a PHP array
 * of connections: peers of other workers would never be reached. The room lives
 * in the server (ws_hub.c) and a broadcast is handed to each worker, which then
 * writes to its own sockets. Here 6 clients spread over 4 workers: one sends, the
 * other five must receive it whichever worker owns them. */
require_once __DIR__ . '/../server/_free_port.inc';
require_once __DIR__ . '/_ws_client.inc';

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\WebSocket;
use TrueAsync\HttpRequest;
use function Async\spawn;
use function Async\delay;

const CLIENTS = 6;

$port = tas_free_port();
$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setWorkers(4)
    ->setReadTimeout(10)
    ->setWriteTimeout(10)
    ->setWsPingIntervalMs(0);

$server = new HttpServer($config);
$room   = $server->room('chat');

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
    delay(4000);   // four workers have to bind

    $conns = [];
    for ($i = 0; $i < CLIENTS; $i++) {
        $fp = ws_open($port);
        if ($fp === null) { echo "handshake failed\n"; posix_kill(getmypid(), SIGKILL); }
        $conns[] = $fp;
    }

    delay(500);    // let every join land

    /* count() is a scatter/gather over the workers, not a shared counter. */
    ws_write($conns[0], 'count');
    delay(1000);
    $tally = ws_read_pending($conns[0]);

    /* Fan out to everyone but the sender — across all four workers. */
    ws_write($conns[0], 'hello');
    delay(1000);

    $got = 0;
    for ($i = 1; $i < CLIENTS; $i++) {
        if (ws_read_pending($conns[$i]) === 'relay:hello') { $got++; }
    }

    $echo = ws_read_pending($conns[0]);

    echo 'count across workers: ', $tally === 'count:' . CLIENTS ? 'yes' : "no ($tally)", "\n";
    echo 'all peers received: ',   $got === CLIENTS - 1 ? 'yes' : "no ($got)", "\n";
    echo 'sender excluded: ',      $echo === '' ? 'yes' : 'no', "\n";

    foreach ($conns as $fp) { fclose($fp); }

    /* The pool parent cannot stop() itself (issue #11) — same exit as 034. */
    posix_kill(getmypid(), SIGKILL);
});

$server->start();
?>
--EXPECTF--
count across workers: yes
all peers received: yes
sender excluded: yes%A
