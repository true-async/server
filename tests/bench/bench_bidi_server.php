<?php
/*
 * Bidi-streaming echo server — minimum-viable validation of the H2
 * streaming stack (PLAN_HTTP2 §Open follow-ups #4 in lieu of a real
 * gRPC bidi bench).
 *
 * Handler pattern: await the request body, then echo it back in
 * 32 KiB chunks via HttpResponse::send(). This exercises:
 *
 *   - H2 DATA-frame ingestion path (cb_on_data_chunk_recv + the
 *     OOM-guarded smart_str preallocation),
 *   - H2 response streaming (DATA-frame emission + WINDOW_UPDATE
 *     round-trip + send-buffer backpressure),
 *   - Admission-reject behaviour on long-lived streams (stream is
 *     alive the whole time, so active_requests stays >= 1 until
 *     dispose).
 *
 * Per-stream it's request/response, not interleaved bidi — but at
 * the connection level H2 multiplexes 100 concurrent streams across
 * one TCP, so DATA frames of both directions interleave on the wire.
 * That is the actual transport property we want to validate under
 * sustained load.
 */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;

$port = (int)($argv[1] ?? 18600);

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setBacklog(512)
    ->setReadTimeout(30)
    ->setWriteTimeout(30)
    ->setMaxBodySize(64 * 1024 * 1024)        /* 64 MiB cap */
    ->setMaxInflightRequests(512)             /* headroom for c=10 m=10 */
    ->setStreamWriteBufferBytes(256 * 1024);  /* 256 KiB backpressure threshold */

$server = new HttpServer($config);

$server->addHttp2Handler(function ($req, $res) {
    /* Wait for the full request body. Our Step 5a streaming-IN is
     * per-stream smart_str accumulation, capped by setMaxBodySize. */
    $req->awaitBody();
    $body = $req->getBody();

    /* Commit status + headers on first send(); everything afterwards
     * is DATA frames (Step 4 streaming-OUT). Chunk at 32 KiB so we
     * exercise WINDOW_UPDATE round-trips — smaller than the default
     * SETTINGS_INITIAL_WINDOW but large enough that we're not wasting
     * syscalls on 1-byte chunks. */
    $res->setStatusCode(200)
        ->setHeader('Content-Type', 'application/octet-stream');

    $len = strlen($body);
    $chunk = 32 * 1024;
    for ($off = 0; $off < $len; $off += $chunk) {
        $res->send(substr($body, $off, $chunk));
    }
    $res->end();
});

/* H1 fallback so an accidental curl probe doesn't hang. */
$server->addHttpHandler(function ($req, $res) {
    $res->setStatusCode(200)->setBody('bidi-bench h1-fallback OK');
});

fprintf(STDERR, "bidi bench server listening on 127.0.0.1:%d (pid %d)\n",
        $port, getmypid());
$server->start();
