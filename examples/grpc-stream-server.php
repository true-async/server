<?php
/**
 * gRPC bidirectional streaming — echo every request message back (#4).
 *
 * Run:
 *   php examples/grpc-stream-server.php
 *   PORT=9000 php examples/grpc-stream-server.php
 *
 * readMessage() deframes the request stream one message at a time;
 * writeMessage() frames each reply. The handler starts on HEADERS, so for
 * true full-duplex you can interleave reads and writes. Call it with a gRPC
 * client, or raw over h2c with two stacked frames ("aaa" then "bb"):
 *
 *   printf '\x00\x00\x00\x00\x03aaa\x00\x00\x00\x00\x02bb' | \
 *     curl --http2-prior-knowledge -s \
 *     -H 'content-type: application/grpc' -H 'te: trailers' --data-binary @- \
 *     http://127.0.0.1:8080/echo.Echo/Stream | xxd
 *   # → two reply frames: "echo: aaa" and "echo: bb"
 *
 * The same shape covers server-streaming (read once, write many) and
 * client-streaming (read many, write once). Runs over HTTP/2 and HTTP/3.
 */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\HttpRequest;
use TrueAsync\HttpResponse;

$config = (new HttpServerConfig())
    ->addListener('0.0.0.0', (int)(getenv('PORT') ?: 8080));

$server = new HttpServer($config);

$server->addGrpcHandler(function (HttpRequest $req, HttpResponse $resp) {
    // Drain the request stream, echoing each message as it is read.
    // In the default (buffered) mode the whole request is already present,
    // so readMessage() returns every message and then null. Enable
    // HttpServerConfig::setBodyStreamingEnabled(true) to read messages as
    // they arrive on the wire (call $req->awaitBody() to block for the tail).
    while (($msg = $req->readMessage()) !== null) {
        $resp->writeMessage("echo: {$msg}");
    }
    // grpc-status:0 defaulted on clean return.
});

$server->addHttpHandler(function (HttpRequest $req, HttpResponse $res) {
    $res->setStatusCode(426)->setBody("This endpoint speaks gRPC.\n");
});

fprintf(STDERR, "[grpc-stream] :%d /echo.Echo/Stream pid=%d\n",
    (int)(getenv('PORT') ?: 8080), getmypid());

$server->start();
