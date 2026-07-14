--TEST--
WebSocket topics: a publish reaches subscribers in every worker, and subscriberCount tallies them
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
/* A worker is a thread with its own PHP context, so a topic cannot be a PHP array
 * of connections: peers of other workers would never be reached. Each worker
 * indexes the connections it owns (ws_topic_tree.c) and a publish is handed to
 * every worker as a plain string, which it matches against its own tree. Here 6
 * clients spread over 4 workers: one publishes, the other five must receive it
 * whichever worker owns them. */
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

/* No handle to obtain, nothing to capture — the connection reaches the hub
 * through the thread that owns it. */
$server->addWebSocketHandler(function (WebSocket $ws, HttpRequest $req) {
    $ws->subscribe('chat');

    foreach ($ws as $msg) {
        if ($msg->data === 'count') {
            $ws->send('count:' . $ws->subscriberCount('chat'));
            continue;
        }

        $ws->publish('chat', 'relay:' . $msg->data);
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

    delay(500);    // let every subscribe land

    /* subscriberCount is a scatter/gather over the workers, not a shared counter. */
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
    echo 'publisher excluded: ',   $echo === '' ? 'yes' : 'no', "\n";

    foreach ($conns as $fp) { fclose($fp); }

    $server->stop();
});

$server->start();
?>
--EXPECTF--
count across workers: yes
all peers received: yes
publisher excluded: yes%A
