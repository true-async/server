<?php
/*
 * Http11Probe target — a plain HTTP/1.1 server for the RFC 9110/9112
 * compliance + request-smuggling + malformed-input suite
 * (https://github.com/MDA2AV/Http11Probe). The probe is target-agnostic:
 * it just needs an h1 server listening on the port. The handler answers
 * GET/POST with a small body and echoes a couple of request facets the
 * probe's caching/normalization checks look at.
 *
 * Usage: php probe_server.php [port]   (default 8080)
 */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;

$port = (int)($argv[1] ?? 8080);

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(10)
    ->setWriteTimeout(10)
    ->setKeepAliveTimeout(30)
    ->setMaxConnections(0);

$server = new HttpServer($config);
$server->addHttpHandler(function ($req, $res) {
    /* Echo the request body and the Cookie header back. Several probe
     * checks (POST-CL-BODY, CHUNKED-*, COOK-ECHO/PARSED-*) assert the
     * server accepts the request AND reflects what it received — a fixed
     * "ok" body fails them even though the server parsed correctly. The
     * echo turns those harness-limited checks into real pass/fail signals. */
    $req->awaitBody();
    $echo = $req->getBody();
    $cookie = $req->getHeader('cookie');
    if ($cookie !== null) {
        $echo .= "\ncookie: " . $cookie;
    }

    /* A stable ETag lets the probe exercise conditional-request caching. */
    $res->setStatusCode(200)
        ->setHeader('Content-Type', 'text/plain')
        ->setHeader('ETag', '"probe-static"')
        ->setBody($echo !== '' ? $echo : "ok\n");
});

$server->start();
