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
const PROBE_LASTMOD  = 'Wed, 21 Oct 2015 07:28:00 GMT';
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

    /* RFC 9110 §13.1.2/3: INM takes precedence over IMS, even on a
     * non-match. Weak comparison applies to the parsed entity-tag list. */
    $inm = $req->getHeader('if-none-match');
    $ims = $req->getHeader('if-modified-since');
    $condition_failed = false;
    if ($inm !== null) {
        $value = trim($inm, " \t");
        if ($value === '*') {
            $condition_failed = true;
        } elseif (preg_match('/^(?:[ \t]*(?:W\/)?"[\x21\x23-\x7E\x80-\xFF]*"[ \t]*,)*[ \t]*(?:W\/)?"[\x21\x23-\x7E\x80-\xFF]*"[ \t]*$/D', $value)) {
            preg_match_all('/(?:W\/)?"([\x21\x23-\x7E\x80-\xFF]*)"/', $value, $tags);
            $condition_failed = in_array(substr(PROBE_ETAG, 1, -1), $tags[1], true);
        }
    } elseif ($ims !== null && ($method === 'GET' || $method === 'HEAD')) {
        /* HTTP-date accepts IMF-fixdate and the two obsolete HTTP forms,
         * but never strtotime's relative expressions (e.g. "tomorrow").
         * A future valid IMS still uses the RFC's lastmod <= IMS rule. */
        $http_date = '/^(?:[A-Z][a-z]{2}, [0-9]{2} [A-Z][a-z]{2} [0-9]{4} [0-9]{2}:[0-9]{2}:[0-9]{2} GMT|[A-Z][a-z]+, [0-9]{2}-[A-Z][a-z]{2}-[0-9]{2} [0-9]{2}:[0-9]{2}:[0-9]{2} GMT|[A-Z][a-z]{2} [A-Z][a-z]{2} [ 0-9][0-9] [0-9]{2}:[0-9]{2}:[0-9]{2} [0-9]{4})$/D';
        if (preg_match($http_date, $ims)) {
            $parts = date_parse($ims);
            $timestamp = strtotime($ims . (str_ends_with($ims, 'GMT') ? '' : ' GMT'));
            $condition_failed = $parts['warning_count'] === 0 && $parts['error_count'] === 0
                && $timestamp !== false && $timestamp >= strtotime(PROBE_LASTMOD);
        }
    }

    if ($condition_failed) {
        $res->setStatusCode($method === 'GET' || $method === 'HEAD' ? 304 : 412)
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
