<?php
/**
 * Minimal handler — equivalent of Swoole's
 *   $http->on('request', fn($req,$res) => $res->end('ok'));
 *
 * Used for fair-apples-to-apples per-thread profiling.
 * Run with WORKERS=1 to compare against Swoole reactor=1+worker=1.
 */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;

$config = (new HttpServerConfig())
    ->addListener('0.0.0.0', (int)(getenv('PORT') ?: 8080))
    ->setBacklog(2048)
    ->setReadTimeout(15)
    ->setWriteTimeout(15)
    ->setKeepAliveTimeout(60);

$server = new HttpServer($config);
$server->addHttpHandler(fn($req, $res) => $res->setBody('ok'));
fprintf(STDERR, "[minimal-server] :%d pid=%d\n", (int)(getenv('PORT') ?: 8080), getmypid());
$server->start();
