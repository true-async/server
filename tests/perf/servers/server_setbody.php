<?php
/* Buffered Response->setBody() — REST API common case.
 *
 * Routes: /b3 /b1k /b16k /b64k /b256k /b1m → body of that exact size.
 * Body bytes are pre-generated in PG memory once so the handler has no
 * per-request alloc.
 */

require __DIR__ . '/_common.php';

use TrueAsync\HttpServer;

[$mode, $port] = perf_parse_mode($argv);

$bodies = [
    '/b3'    => str_repeat('x', 3),
    '/b1k'   => str_repeat('x', 1024),
    '/b16k'  => str_repeat('x', 16 * 1024),
    '/b64k'  => str_repeat('x', 64 * 1024),
    '/b256k' => str_repeat('x', 256 * 1024),
    '/b1m'   => str_repeat('x', 1024 * 1024),
];

$server = new HttpServer(perf_make_config($mode, $port));
$server->addHttpHandler(function ($req, $resp) use ($bodies) {
    $body = $bodies[$req->getPath()] ?? "OK\n";
    $resp->setStatusCode(200)
         ->setHeader('Content-Type', 'application/octet-stream')
         ->setBody($body);
});

fprintf(STDERR, "perf:setbody mode=%s port=%d pid=%d\n", $mode, $port, getmypid());
$server->start();
