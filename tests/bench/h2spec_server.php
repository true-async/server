<?php
/*
 * h2spec target — bare HTTP/2 server for compliance testing.
 *
 * h2spec drives a compliance matrix against an h2 endpoint. Our
 * handler answers the minimal set of URIs h2spec sends requests to
 * with a generic 200. Bodies are irrelevant for framing/HPACK/flow-
 * control compliance tests; h2spec only cares that we respond with
 * valid HTTP/2 framing.
 */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;


$port = (int)($argv[1] ?? 18081);

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(10)
    ->setWriteTimeout(10)
    ->setKeepAliveTimeout(30)
    ->setMaxConnections(0);

$server = new HttpServer($config);
/* h2-only: registering addHttp2Handler (and NOT addHttpHandler) tells
 * the detector that HTTP/1 is not served on this listener. Garbage
 * that isn't a valid h2 preface now routes to the h2 strategy's
 * bad-preface error path (GOAWAY PROTOCOL_ERROR + close) instead of
 * falling through to HTTP/1, which is what h2spec http2/3.5/2 tests. */
$server->addHttp2Handler(function ($req, $res) {
    $res->setStatusCode(200)
        ->setHeader('Content-Type', 'text/plain')
        ->setBody("ok\n");
});

$server->start();
