<?php
/*
 * Upload throughput bench server — drains request body, replies with
 * received byte count. Used by tests/bench/run_bench_h2_upload.sh for
 * 1 GiB POST benchmarks (PLAN_HTTP2 Step 10, upload throughput target).
 */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;

$port = (int)($argv[1] ?? 18080);

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setBacklog(512)
    ->setReadTimeout(300)
    ->setWriteTimeout(300)
    ->setMaxBodySize(2 * 1024 * 1024 * 1024);   /* 2 GiB */

$server = new HttpServer($config);

$server->addHttpHandler(function($request, $response) {
    $request->awaitBody();
    $body = $request->getBody();
    $len  = $body !== null ? strlen($body) : 0;
    $response->setStatusCode(200);
    $response->setHeader('Content-Type', 'text/plain');
    $response->setBody("received=$len\n");
});

fprintf(STDERR, "upload bench server listening on 127.0.0.1:%d (pid %d)\n",
    $port, getmypid());
$server->start();
