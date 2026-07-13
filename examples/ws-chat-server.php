<?php
/**
 * WebSocket chat — one topic, many workers.
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
 *   - A topic is addressed by NAME, at the call site. There is no topic object to
 *     obtain, hold or pass into the handler — the connection reaches the hub
 *     through the thread that owns it, so nothing has to be captured.
 *   - publish() never suspends: a peer whose socket is backed up drops the
 *     message instead of stalling delivery to the whole topic.
 *   - subscriberCount() asks every worker and sums the answers — a snapshot, not
 *     a live counter.
 *
 * A worker is a thread with its own PHP context, so an array of connections
 * could only ever reach that worker's peers. Topics live in the server: each
 * worker indexes the connections it owns, and a publish is handed to every
 * worker, which delivers to its own sockets. No Redis, no setWorkers(1).
 *
 * Filters follow MQTT, so a client could subscribe to `chat/#` and receive every
 * room at once, or `chat/+/typing` for the typing indicator of any room.
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

$server->addWebSocketHandler(function (WebSocket $ws, HttpRequest $req) {
    $ws->subscribe('chat');   // unsubscribing is automatic when the connection closes

    $ws->send('welcome — ' . $ws->subscriberCount('chat') . ' online');

    foreach ($ws as $msg) {
        $line = ($msg->binary ? '[binary] ' : '') . $msg->data;

        // Fan out to everyone except the sender — across all four workers.
        $ws->publish('chat', $line);
    }
});

$server->addHttpHandler(function (HttpRequest $req, HttpResponse $res) {
    $res->setStatusCode(426)->setBody("Connect with a WebSocket client.\n");
});

fprintf(STDERR, "[ws-chat] ws://127.0.0.1:%d/ pid=%d\n",
    (int)(getenv('PORT') ?: 8080), getmypid());

$server->start();
