<?php
/**
 * WebSocket echo server — the "hello world" of WebSocket (RFC 6455).
 *
 * Run:
 *   php examples/ws-echo-server.php            # listens on :8080
 *   PORT=9000 php examples/ws-echo-server.php
 *
 * Connect with any client:
 *   websocat ws://127.0.0.1:8080/
 *   # then type a line and it comes straight back
 *
 * Browser console:
 *   const ws = new WebSocket('ws://127.0.0.1:8080/');
 *   ws.onmessage = e => console.log('echo:', e.data);
 *   ws.onopen    = () => ws.send('hello');
 *
 * The same handler works unchanged over wss:// (TLS) and HTTP/2 Extended
 * CONNECT (RFC 8441) — the transport is chosen by the client.
 */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\WebSocket;
use TrueAsync\HttpRequest;
use TrueAsync\HttpResponse;

$config = (new HttpServerConfig())
    ->addListener('0.0.0.0', (int)(getenv('PORT') ?: 8080));

$server = new HttpServer($config);

$server->addWebSocketHandler(function (WebSocket $ws, HttpRequest $req) {
    // The handler runs in its own coroutine. `foreach` pulls one fully
    // reassembled message at a time and ends on a clean close; returning
    // from the handler closes the socket with 1000 Normal.
    foreach ($ws as $msg) {
        if ($msg->binary) {
            $ws->sendBinary($msg->data);   // echo binary as binary
        } else {
            $ws->send($msg->data);         // echo text as text
        }
    }
});

// A WebSocket server still needs an HTTP handler for non-upgrade requests.
$server->addHttpHandler(function (HttpRequest $req, HttpResponse $res) {
    $res->setStatusCode(426)->setBody("Connect with a WebSocket client.\n");
});

fprintf(STDERR, "[ws-echo] ws://127.0.0.1:%d/ pid=%d\n",
    (int)(getenv('PORT') ?: 8080), getmypid());

$server->start();
