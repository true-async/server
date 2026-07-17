--TEST--
Rooms cross-worker: HttpServer::publish() from a coroutine fans out to subscribers owned by every worker; HttpServer::subscriberCount() tallies across workers from a handler
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
/* The server-side publish crossing worker threads: 6 clients spread over 4
 * workers all subscribe to one topic; a coroutine that owns no connection calls
 * $server->publish() once, and every client must receive it whichever worker
 * owns it. Because the publisher is the server (no ws_id), nobody is excluded —
 * all 6 receive.
 *
 * subscriberCount() is a scatter/gather that collects replies on the caller's
 * hub mailbox, so it must run on an attached worker thread — here the handler
 * answers a 'count' request with $server->subscriberCount(), proving the
 * server-level count works across workers from within a worker. */
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
$server->enableRooms();

$server->addWebSocketHandler(function (WebSocket $ws, HttpRequest $req) use ($server) {
    $ws->subscribe('chat');

    foreach ($ws as $msg) {
        if ($msg->data === 'count') {
            /* server-level count, evaluated on this worker (attached thread) */
            $ws->send('count:' . $server->subscriberCount('chat'));
        }
    }
});

$server->addHttpHandler(function ($req, $res) { $res->setStatusCode(404)->end(); });

spawn(function () use ($port, $server) {
    delay(4000);   // four workers have to bind

    $conns = [];
    for ($i = 0; $i < CLIENTS; $i++) {
        $fp = ws_open($port);
        if ($fp === null) { echo "handshake failed\n"; $server->stop(); return; }
        $conns[] = $fp;
    }

    delay(600);    // let every subscribe land

    /* server-level subscriberCount, evaluated on a worker via the handler */
    ws_write($conns[0], 'count');
    delay(1000);
    $tally = ws_read_pending($conns[0]);
    echo 'count across workers: ', $tally === 'count:' . CLIENTS ? 'yes' : "no ($tally)", "\n";

    /* one server-side publish from this (parent) coroutine reaches everyone */
    $server->publish('chat', 'broadcast');
    delay(1000);

    $got = 0;
    for ($i = 0; $i < CLIENTS; $i++) {
        if (ws_read_pending($conns[$i]) === 'broadcast') { $got++; }
    }
    echo 'all peers received: ', $got === CLIENTS ? 'yes' : "no ($got)", "\n";

    foreach ($conns as $fp) { fclose($fp); }

    $server->stop();
});

$server->start();
?>
--EXPECTF--
count across workers: yes
all peers received: yes%A
