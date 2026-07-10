<?php
/**
 * WebSocket broadcast chat room — shows the multi-producer send model.
 *
 * Run (single worker keeps one shared room in-process):
 *   php examples/ws-chat-server.php
 *   PORT=9000 php examples/ws-chat-server.php
 *
 * Open two terminals and watch messages fan out between them:
 *   websocat ws://127.0.0.1:8080/
 *   websocat ws://127.0.0.1:8080/
 *
 * Key ideas:
 *   - One coroutine per connection; `foreach ($ws as $msg)` is the pull loop.
 *   - send() is safe to call from ANY coroutine on the thread, so one
 *     connection's handler can push to every other peer's socket.
 *   - trySend() never suspends — it drops for a backpressured (slow) peer
 *     instead of stalling delivery to the whole room. Use send() instead if
 *     you need every message delivered and are willing to apply backpressure.
 *
 * Scaling past one worker: each worker is a separate thread with its own
 * $room, so a cross-worker chat needs shared state (Redis pub/sub, etc.).
 * This example uses setWorkers(1) to keep the room in one process.
 */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\WebSocket;
use TrueAsync\HttpRequest;
use TrueAsync\HttpResponse;

$config = (new HttpServerConfig())
    ->addListener('0.0.0.0', (int)(getenv('PORT') ?: 8080))
    ->setWorkers(1);

$server = new HttpServer($config);

/** @var \SplObjectStorage<WebSocket,true> $room */
$room = new \SplObjectStorage();

$server->addWebSocketHandler(function (WebSocket $ws, HttpRequest $req) use ($room) {
    $room->attach($ws);
    $ws->send('welcome — ' . $room->count() . ' online');

    try {
        foreach ($ws as $msg) {
            $line = ($msg->binary ? '[binary] ' : '') . $msg->data;

            // Fan out to everyone except the sender.
            foreach ($room as $peer) {
                if ($peer !== $ws) {
                    $peer->trySend($line);
                }
            }
        }
    } finally {
        $room->detach($ws);
    }
});

$server->addHttpHandler(function (HttpRequest $req, HttpResponse $res) {
    $res->setStatusCode(426)->setBody("Connect with a WebSocket client.\n");
});

fprintf(STDERR, "[ws-chat] ws://127.0.0.1:%d/ pid=%d\n",
    (int)(getenv('PORT') ?: 8080), getmypid());

$server->start();
