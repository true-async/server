<?php
/*
 * Minimal HTTP handler for benchmarking. Returns "OK\n" with a fixed
 * Content-Length so PHP-side work is minimal — we're measuring extension
 * and reactor overhead, not user-handler latency.
 */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;

$port = (int)($argv[1] ?? 18080);

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setBacklog(512)
    ->setReadTimeout(5)
    ->setWriteTimeout(5);

$server = new HttpServer($config);

$server->addHttpHandler(function($request, $response) {
    $response->setStatusCode(200);
    $response->setHeader('Content-Type', 'text/plain');
    $response->setBody("OK\n");
});

fprintf(STDERR, "bench server listening on 127.0.0.1:%d (pid %d)\n", $port, getmypid());
$server->start();
