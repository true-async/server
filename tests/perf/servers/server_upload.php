<?php
/* POST body echo — exercises the inbound parser + body buffering on
 * the request path. Echoes a fixed-size "OK" response so the variable
 * being measured is upload throughput, not response size.
 *
 * Route: POST / with arbitrary body, any size up to extension's
 *        HTTP2_MAX_BODY_SIZE (10 MiB by default).
 */

require __DIR__ . '/_common.php';

use TrueAsync\HttpServer;

[$mode, $port] = perf_parse_mode($argv);

$server = new HttpServer(perf_make_config($mode, $port));
$server->addHttpHandler(function ($req, $resp) {
    if ($req->getMethod() !== 'POST') {
        $resp->setStatusCode(405)->setBody("POST only\n");
        return;
    }
    $body = $req->getBody() ?? '';
    $resp->setStatusCode(200)
         ->setHeader('Content-Length', (string)strlen($body))
         ->setHeader('X-Echo-Length', (string)strlen($body))
         ->setBody("OK\n");
});

fprintf(STDERR, "perf:upload mode=%s port=%d pid=%d\n", $mode, $port, getmypid());
$server->start();
