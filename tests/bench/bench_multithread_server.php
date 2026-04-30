<?php
/*
 * Multi-worker HTTP server for benchmarking REUSEPORT scaling.
 *
 * Main thread: builds one HttpServer + handler closure, then submits
 * $workers tasks to a ThreadPool. Each task calls $server->start() in
 * its own PHP thread. The server is passed by transfer_obj — worker
 * gets a fresh HttpServer pointing at the same frozen config and a
 * re-instantiated handler closure. Each worker binds its own listen
 * socket with SO_REUSEPORT, so the kernel load-balances accept()s
 * across workers.
 *
 * Usage: php bench_multithread_server.php [port] [workers]
 */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use Async\ThreadPool;
use function Async\spawn;

$port    = (int)($argv[1] ?? 18080);
$workers = (int)($argv[2] ?? 4);

spawn(function() use ($port, $workers) {
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

    fprintf(STDERR, "bench: %d workers on 127.0.0.1:%d (pid %d)\n",
        $workers, $port, getmypid());

    $pool = new ThreadPool($workers);

    // Fire-and-forget: each worker blocks in start() until the process
    // is killed by the harness.
    for ($i = 0; $i < $workers; $i++) {
        $pool->submit(function() use ($server) {
            $server->start();
        });
    }

    // Main coroutine parks forever; harness sends SIGTERM when done.
    while (true) {
        \Async\delay(1000);
    }
});
