<?php
/**
 * Server-Sent Events (text/event-stream) demo.
 *
 * Open http://127.0.0.1:8080/events in a browser tab, or:
 *   curl -N http://127.0.0.1:8080/events
 *
 * The same handler works unchanged over HTTP/1.1, HTTP/2 and HTTP/3 —
 * SSE is just the text/event-stream framing layered on the streaming
 * response pipeline, so the protocol is chosen by the client.
 *
 * Browser side:
 *   const es = new EventSource('/events');
 *   es.onmessage      = e => console.log('message', e.data);
 *   es.addEventListener('tick', e => console.log('tick', e.data, e.lastEventId));
 */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\delay;

$config = (new HttpServerConfig())
    ->addListener('0.0.0.0', (int)(getenv('PORT') ?: 8080))
    ->setWriteTimeout(0);   // long-lived stream: no write deadline

$server = new HttpServer($config);

$server->addHttpHandler(function ($req, $res) {
    // Commit the SSE headers (Content-Type, Cache-Control, X-Accel-Buffering).
    // Optional — the first sseEvent()/sseComment() starts the stream too.
    $res->sseStart();

    // Hint the browser to wait 3s before reconnecting after a drop.
    $res->sseRetry(3000);

    // Comment line = heartbeat that keeps proxies from idling the conn out.
    $res->sseComment('stream open');

    for ($i = 1; $i <= 10; $i++) {
        // A named event with an id (echoed back as Last-Event-ID on reconnect).
        $res->sseEvent(
            data:  json_encode(['n' => $i, 'at' => time()]),
            event: 'tick',
            id:    (string) $i,
        );

        // sendable() is an advisory backpressure check — skip the sleep and
        // bail early if the peer has gone away.
        if (!$res->sendable()) {
            break;
        }

        delay(1000);   // 1s between events (cooperative, non-blocking)
    }

    // A default (unnamed) message event, then close the stream.
    $res->sseEvent('bye');
    $res->end();
});

fprintf(STDERR, "[sse-server] http://127.0.0.1:%d/events pid=%d\n",
    (int)(getenv('PORT') ?: 8080), getmypid());

$server->start();
