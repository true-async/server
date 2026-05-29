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
const PROBE_ETAG     = '"probe-static"';
const PROBE_LASTMOD  = 'Wed, 21 Oct 2026 07:28:00 GMT';
const PROBE_ALLOW    = 'GET, HEAD, POST, OPTIONS';

$server->addHttpHandler(function ($req, $res) {
    $method = $req->getMethod();

    /* OPTIONS → advertise the allowed methods (COMP-OPTIONS-ALLOW). */
    if ($method === 'OPTIONS') {
        $res->setStatusCode(200)->setHeader('Allow', PROBE_ALLOW)->setBody('');
        return;
    }

    /* A method outside the supported set → 405 + Allow (COMP-405-ALLOW). */
    if (!in_array($method, ['GET', 'HEAD', 'POST'], true)) {
        $res->setStatusCode(405)->setHeader('Allow', PROBE_ALLOW)->setBody('');
        return;
    }

    /* Conditional requests → 304 (CAP-ETAG-304 / INM-* / LAST-MODIFIED-304).
     * If-None-Match matches our ETag (or "*"), or If-Modified-Since is at
     * or after our Last-Modified → not modified. */
    $inm = $req->getHeader('if-none-match');
    $ims = $req->getHeader('if-modified-since');
    $not_modified =
        ($inm !== null && (trim($inm) === '*' || str_contains($inm, PROBE_ETAG)))
        || ($ims !== null && ($t = strtotime($ims)) !== false
            && $t >= strtotime(PROBE_LASTMOD));

    if ($not_modified) {
        $res->setStatusCode(304)
            ->setHeader('ETag', PROBE_ETAG)
            ->setHeader('Last-Modified', PROBE_LASTMOD)
            ->setBody('');
        return;
    }

    /* Echo the request body and the Cookie header back. Several probe
     * checks (POST-CL-BODY, CHUNKED-*, COOK-ECHO/PARSED-*) assert the
     * server accepts the request AND reflects what it received — a fixed
     * "ok" body fails them even though the server parsed correctly. */
    $req->awaitBody();
    $echo = $req->getBody();
    $cookie = $req->getHeader('cookie');
    if ($cookie !== null) {
        $echo .= "\ncookie: " . $cookie;
    }

    $res->setStatusCode(200)
        ->setHeader('Content-Type', 'text/plain')
        ->setHeader('ETag', PROBE_ETAG)
        ->setHeader('Last-Modified', PROBE_LASTMOD)
        ->setBody($echo !== '' ? $echo : "ok\n");
});

$server->start();
