<?php
/**
 * Echo server under test for the Autobahn|Testsuite fuzzingclient.
 *
 * Mirrors the canonical Autobahn echo server: every inbound message is
 * sent straight back, preserving text/binary framing. permessage-deflate
 * is enabled so the compression cases (12.* / 13.*) exercise RFC 7692.
 *
 * Usage:  php server.php [port]
 * The wstest container connects to ws://<host>:<port>/ and drives ~500
 * conformance cases against this handler.
 */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\WebSocket;
use TrueAsync\WebSocketException;
use TrueAsync\HttpRequest;

$port = (int)($argv[1] ?? 9001);

$config = (new HttpServerConfig())
    ->addListener('0.0.0.0', $port)
    ->setReadTimeout(30)
    ->setWriteTimeout(30)
    // Autobahn pushes 16 MiB payloads in the 9.* / 10.* cases; raise the
    // caps so legitimate large frames are echoed instead of 1009'd.
    ->setWsMaxMessageSize(20 * 1024 * 1024)
    ->setWsMaxFrameSize(20 * 1024 * 1024)
    ->setWsPermessageDeflate(true);

$server = new HttpServer($config);

$server->addWebSocketHandler(function (WebSocket $ws, HttpRequest $req): void {
    // Autobahn deliberately sends protocol violations (reserved bits,
    // bad UTF-8, oversized control frames). The server closes the
    // connection on those, after which recv()/send() throw — catch it
    // so one misbehaving connection can never take the whole server
    // down. A real application would catch WebSocketException the same way.
    try {
        while (($msg = $ws->recv()) !== null) {
            if ($msg->binary) {
                $ws->sendBinary($msg->data);
            } else {
                $ws->send($msg->data);
            }
        }
    } catch (WebSocketException $e) {
        // Connection closed mid-exchange — end this handler cleanly.
    }
});

// Plain HTTP for the /ping liveness probe run.sh uses before starting.
$server->addHttpHandler(function ($req, $resp): void {
    $resp->setStatusCode(200)->end('autobahn echo server');
});

fwrite(STDERR, "autobahn echo server listening on :$port\n");
$server->start();
