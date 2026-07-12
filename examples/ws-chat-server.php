<?php
/**
 * WebSocket chat room — one room, many workers.
 *
 * Run:
 *   php examples/ws-chat-server.php
 *   PORT=9000 php examples/ws-chat-server.php
 *
 * Open two terminals and watch messages fan out between them:
 *   websocat ws://127.0.0.1:8080/
 *   websocat ws://127.0.0.1:8080/
 *
 * Key ideas:
 *   - One coroutine per connection; `foreach ($ws as $msg)` is the pull loop.
 *   - broadcast() never suspends: a peer whose socket is backed up drops the
 *     message instead of stalling delivery to the whole room.
 *   - count() asks every worker and sums the answers — a snapshot, not a live
 *     counter.
 *
 * The room is held by the server, not by PHP: a worker is a thread with its own
 * PHP context, so an array of connections could only ever reach that worker's
 * peers. HttpServer::room() gives every worker the same room, and broadcast()
 * reaches members wherever they are — no Redis, no setWorkers(1).
 */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\WebSocket;
use TrueAsync\HttpRequest;
use TrueAsync\HttpResponse;

$config = (new HttpServerConfig())
    ->addListener('0.0.0.0', (int)(getenv('PORT') ?: 8080))
    ->setWorkers(4);

$server = new HttpServer($config);

$room = $server->room('chat');

$server->addWebSocketHandler(function (WebSocket $ws, HttpRequest $req) use ($room) {
    $room->join($ws);   // leaving is automatic when the connection closes
    $ws->send('welcome — ' . $room->count() . ' online');

    foreach ($ws as $msg) {
        $line = ($msg->binary ? '[binary] ' : '') . $msg->data;

        // Fan out to everyone except the sender — across all four workers.
        $room->broadcast($line, $ws);
    }
});

$server->addHttpHandler(function (HttpRequest $req, HttpResponse $res) {
    $res->setStatusCode(426)->setBody("Connect with a WebSocket client.\n");
});

fprintf(STDERR, "[ws-chat] ws://127.0.0.1:%d/ pid=%d\n",
    (int)(getenv('PORT') ?: 8080), getmypid());

$server->start();
