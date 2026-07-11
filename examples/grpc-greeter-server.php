<?php
/**
 * gRPC unary greeter — one request message in, one reply message out (#4).
 *
 * Run:
 *   php examples/grpc-greeter-server.php
 *   PORT=9000 php examples/grpc-greeter-server.php
 *
 * gRPC messages are your protobuf payloads; the 5-byte length-prefix framing
 * is handled by read/writeMessage(). Call it with any gRPC client (grpcurl,
 * generated stubs), or raw over h2c — the frame is 1 flag byte + 4-byte
 * big-endian length + payload:
 *
 *   printf '\x00\x00\x00\x00\x05world' | curl --http2-prior-knowledge -s \
 *     -H 'content-type: application/grpc' -H 'te: trailers' --data-binary @- \
 *     http://127.0.0.1:8080/helloworld.Greeter/SayHello | tail -c +6; echo
 *   # → hello, world
 *
 * Runs unchanged over HTTP/2 and HTTP/3.
 */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\HttpRequest;
use TrueAsync\HttpResponse;

$config = (new HttpServerConfig())
    ->addListener('0.0.0.0', (int)(getenv('PORT') ?: 8080));

$server = new HttpServer($config);

$server->addGrpcHandler(function (HttpRequest $req, HttpResponse $resp) {
    $name = $req->readMessage() ?? '';       // deframed request message
    $resp->writeMessage("hello, {$name}");   // framed reply

    // grpc-status:0 (OK) is defaulted on a clean return. To fail the RPC:
    //   $resp->setTrailer('grpc-status', '5')       // 5 = NOT_FOUND
    //        ->setTrailer('grpc-message', 'no such greeting');
});

// gRPC shares the listener with plain HTTP; a fallback handler is required.
$server->addHttpHandler(function (HttpRequest $req, HttpResponse $res) {
    $res->setStatusCode(426)->setBody("This endpoint speaks gRPC.\n");
});

fprintf(STDERR, "[grpc-greeter] :%d /helloworld.Greeter/SayHello pid=%d\n",
    (int)(getenv('PORT') ?: 8080), getmypid());

$server->start();
